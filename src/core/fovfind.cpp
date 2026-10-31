// Debug (DebugFindGameFov = 1): find where the game keeps its camera field of view. Scans private RW memory for the
// exact float the game sends as CameraNodeProxyData +0x104 (vertical FOV), then tries the hits one by one on the game
// thread: write FOV x 1.4, wait a few frames, see whether the FOV the game sends changes; restore either way.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include "core.h"
#include "common/log.h"
#include "common/config.h"

namespace svr {

static int g_req = 0;
static volatile bool g_scanDone = false, g_scanning = false;
static std::vector<uint8_t*> g_hits;
static uint32_t g_fovBits = 0;
static size_t g_idx = 0;
static int g_wait = 0;
static float g_before = 0;
static bool g_testing = false;

void FovFind_LoadSettings(const xrb::Config& c) { g_req = c.GetInt("DebugFindGameFov", 0); }

static DWORD WINAPI ScanFov(void*) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uint8_t* a = (uint8_t*)si.lpMinimumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    static uint32_t buf[1 << 18];
    while (a < (uint8_t*)si.lpMaximumApplicationAddress && VirtualQuery(a, &mbi, sizeof(mbi))) {
        uint8_t* next = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            uint8_t* rb = (uint8_t*)mbi.BaseAddress;
            size_t left = mbi.RegionSize;
            while (left && g_hits.size() < 400) {
                size_t chunk = left < sizeof(buf) ? left : sizeof(buf);
                SIZE_T got = 0;
                if (ReadProcessMemory(GetCurrentProcess(), rb, buf, chunk, &got) && got == chunk)
                    for (size_t i = 0; i < chunk / 4; i++) if (buf[i] == g_fovBits) g_hits.push_back(rb + i * 4);
                rb += chunk; left -= chunk;
            }
        }
        a = next;
    }
    LOGI("fovfind: %zu places hold the game FOV value 0x%08x", g_hits.size(), g_fovBits);
    g_scanDone = true;
    return 0;
}

void FovFind_Frame() {
    if (g_req <= 0) return;
    float fov = Stereo_GameFov();
    if (fov <= 0) return;
    if (!g_scanning) {
        g_scanning = true;
        memcpy(&g_fovBits, &fov, 4);
        HANDLE h = CreateThread(nullptr, 0, ScanFov, nullptr, 0, nullptr);
        if (h) CloseHandle(h);
        return;
    }
    if (!g_scanDone) return;
    if (g_testing) {
        if (--g_wait > 0) return;
        float now = Stereo_GameFov();
        uint8_t* p = g_hits[g_idx];
        WriteProcessMemory(GetCurrentProcess(), p, &g_fovBits, 4, nullptr);   // restore
        g_testing = false;
        if (fabsf(now - g_before) > 0.05f) {
            uint8_t around[128] = {};
            ReadProcessMemory(GetCurrentProcess(), p - 64, around, sizeof(around), nullptr);
            char line[1200]; int k = snprintf(line, sizeof(line), "fovfind: FOUND game FOV at %p (sent FOV %.4f -> %.4f). around:", p, g_before, now);
            for (int i = 0; i < 32; i++) { float f; memcpy(&f, around + i * 4, 4); k += snprintf(line + k, sizeof(line) - k, " %.4g", f); }
            LOGI("%s", line);
        }
        g_idx++;
        return;
    }
    if (g_idx >= g_hits.size()) { LOGI("fovfind: done"); g_req = 0; return; }
    g_before = Stereo_GameFov();
    float nv = g_before * 1.4f;
    if (WriteProcessMemory(GetCurrentProcess(), g_hits[g_idx], &nv, 4, nullptr)) { g_testing = true; g_wait = 4; }
    else g_idx++;
}

} // namespace svr
