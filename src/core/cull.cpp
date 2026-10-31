// Culling: the game decides what to draw from its own camera, which does not turn with the head, so objects outside
// the flat-screen view vanish when you look to the side. Widening the game camera's FOV widens everything the game
// derives from it (culling, streaming / relevance) without changing the VR image (the headset projection replaces the
// game's). CullWiden = how much wider (tangent of the half angle x this; 1 = off).
//
// The game camera keeps { aspect, aspect, aspect, vertical FOV (rad), near, far } (the aspects are not always
// identical: 3840x2141 window -> only the last one is checked) as consecutive floats (found by
// writing a candidate and watching the FOV the game sends, CameraNodeProxyData +0x104). A background scan finds that
// pattern with the values the game currently sends; then each frame the FOV is set to the widened value. The original
// is kept in LoaderApi::persist[3] across core reloads and written back on unload.
// (The Umbra/DPVS culling cameras were tried first: changing their frustum has no visible effect.)
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <vector>
#include "core.h"
#include "common/log.h"
#include "common/config.h"

namespace svr {

static float g_widen = 30.0f;
static float g_aspectBoost = 1.0f;            // CullAspect: horizontal-only widening (game camera aspect values x this)
static uint8_t* volatile g_fovAddr[4];          // address of the FOV float
static volatile int g_nAddr = 0;
static volatile bool g_scanning = false;
static DWORD g_lastScan = 0;
static uint32_t g_patFov = 0, g_patNear = 0, g_patFar = 0;

static int g_watchSec = 0;     // DebugWatchCullFov = N: read/write watch on the camera FOV for N seconds (who reads it)
static DWORD g_watchT0 = 0, g_watchLast = 0;

void Cull_LoadSettings(const xrb::Config& c) {
    g_watchSec = c.GetInt("DebugWatchCullFov", 0);
    CullSites_LoadSettings(c);
    g_widen = c.GetFloat("CullWiden", 30.0f);
    g_aspectBoost = 1.0f;   // CullAspect removed: no effect on culling, breaks the sky fix (kept off)
    if (g_aspectBoost < 1.0f) g_aspectBoost = 1.0f;
    if (g_aspectBoost > 3.0f) g_aspectBoost = 3.0f;
    if (g_widen < 1.0f) g_widen = 1.0f;
    if (g_widen > 30.0f) g_widen = 30.0f;
}
void Cull_SetWiden(float w) { g_widen = w < 1.f ? 1.f : (w > 30.f ? 30.f : w); }
void Cull_SetAspect(float) {}

// original aspect values of the game camera (persist[4..6], float bits; 0 = not captured)
static float OrigAsp(int i) { uint32_t b = (uint32_t)(uintptr_t)g_api->persist[4 + i]; float f; memcpy(&f, &b, 4); return b ? f : 0.f; }
static void SetOrigAsp(int i, float f) { uint32_t b; memcpy(&b, &f, 4); g_api->persist[4 + i] = (void*)(uintptr_t)b; }
void Cull_Tick() {}

static float OrigFov() {
    uint32_t b = (uint32_t)(uintptr_t)g_api->persist[3];
    float f; memcpy(&f, &b, 4);
    return b ? f : 0.f;
}
static void SetOrigFov(float f) { uint32_t b; memcpy(&b, &f, 4); g_api->persist[3] = (void*)(uintptr_t)b; }

static DWORD WINAPI ScanThread(void*) {
    SYSTEM_INFO si; GetSystemInfo(&si);
    uint8_t* a = (uint8_t*)si.lpMinimumApplicationAddress;
    MEMORY_BASIC_INFORMATION mbi;
    static uint32_t buf[1 << 18];   // 1 MB
    std::vector<uint8_t*> found;
    size_t scanned = 0;
    while (a < (uint8_t*)si.lpMaximumApplicationAddress && VirtualQuery(a, &mbi, sizeof(mbi))) {
        uint8_t* next = (uint8_t*)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) {
            uint8_t* rb = (uint8_t*)mbi.BaseAddress;
            size_t left = mbi.RegionSize;
            while (left && found.size() < 4) {
                size_t chunk = left < sizeof(buf) ? left : sizeof(buf);
                SIZE_T got = 0;
                if (ReadProcessMemory(GetCurrentProcess(), rb, buf, chunk, &got) && got == chunk) {
                    size_t n = chunk / 4;
                    for (size_t i = 3; i + 2 < n; i++) {
                        if (buf[i] != g_patFov || buf[i + 1] != g_patNear || buf[i + 2] != g_patFar) continue;
                        float a3[3]; memcpy(a3, &buf[i - 3], 12);   // aspect-like values before it (not always identical)
                        if (!(a3[2] > 0.3f && a3[2] < 15.f)) continue;
                        LOGI("cull: candidate %p: %.4f %.4f %.4f | fov near far", rb + i * 4, a3[0], a3[1], a3[2]);
                        found.push_back(rb + i * 4);
                    }
                    scanned += chunk;
                }
                rb += chunk; left -= chunk;
            }
        }
        a = next;
    }
    for (size_t i = 0; i < found.size(); i++) g_fovAddr[i] = found[i];
    g_nAddr = (int)found.size();
    LOGI("cull: game camera FOV found in %d place(s) (scanned %zu MB)%s", g_nAddr, scanned >> 20, g_nAddr ? "" : " - culling boost unavailable for now");
    g_scanning = false;
    return 0;
}

static void StartScan(float fov, float nearZ, float farZ) {
    if (g_scanning) return;
    g_scanning = true;
    g_lastScan = GetTickCount();
    memcpy(&g_patFov, &fov, 4); memcpy(&g_patNear, &nearZ, 4); memcpy(&g_patFar, &farZ, 4);
    HANDLE h = CreateThread(nullptr, 0, ScanThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h); else g_scanning = false;
}

// Write the FOV (and, when `aspects` is given, the three aspect values) to every camera copy still valid.
// `captureAsp`: store the current aspect values as the originals first (once).
static void WriteAll(float v, const float* aspects = nullptr, bool captureAsp = false) {
    int lost = 0;
    for (int i = 0; i < g_nAddr; i++) {
        uint8_t* p = g_fovAddr[i];
        if (!p) continue;
        uint32_t w[6];   // aspect x3, fov, near, far: still the game camera?
        float cur, a2;
        bool ok = ReadProcessMemory(GetCurrentProcess(), p - 12, w, sizeof(w), nullptr) != 0;
        if (ok) memcpy(&a2, &w[2], 4);
        if (!ok || !(a2 > 0.3f && a2 < 15.f) || w[4] != g_patNear || w[5] != g_patFar) { g_fovAddr[i] = nullptr; lost++; continue; }
        memcpy(&cur, &w[3], 4);
        if (!(cur > 0.1f && cur < 3.0f)) { g_fovAddr[i] = nullptr; lost++; continue; }
        if (captureAsp && i == 0 && OrigAsp(0) == 0.f) {
            float a[3]; memcpy(a, w, 12);
            for (int k = 0; k < 3; k++) SetOrigAsp(k, a[k]);
            LOGI("cull: game camera aspect values %.4f %.4f %.4f", a[0], a[1], a[2]);
        }
        if (cur != v) WriteProcessMemory(GetCurrentProcess(), p, &v, 4, nullptr);
        if (aspects) {
            float a[3]; memcpy(a, w, 12);
            if (memcmp(a, aspects, 12) != 0) WriteProcessMemory(GetCurrentProcess(), p - 12, aspects, 12, nullptr);
        }
    }
    if (lost) {
        int n = 0;
        for (int i = 0; i < g_nAddr; i++) if (g_fovAddr[i]) g_fovAddr[n++] = g_fovAddr[i];
        g_nAddr = n;
        LOGI("cull: game camera moved (zone change?) - searching again");
    }
}

// Every frame on the game thread.
void Cull_Frame() {
    if (g_nAddr) {   // near/far of the camera we found (they may change with the zone: then search again)
        float n2, f2;
        if (Stereo_GameNearFar(&n2, &f2)) { uint32_t a, b; memcpy(&a, &n2, 4); memcpy(&b, &f2, 4); if (a != g_patNear || b != g_patFar) { g_nAddr = 0; } }
    }
    float sent = Stereo_GameFov(), nearZ, farZ;
    if (sent <= 0 || !Stereo_GameNearFar(&nearZ, &farZ)) return;
    bool want = (g_widen > 1.001f || g_aspectBoost > 1.001f) && !Vr_FlatMode();   // flat mode shows the game camera: keep it
    float orig = OrigFov();
    if (!want || !g_nAddr) CullSites_Pause();
    if (!want) {
        if (orig > 0 && g_nAddr) {
            float oa[3] = { OrigAsp(0), OrigAsp(1), OrigAsp(2) };
            WriteAll(orig, oa[0] > 0 ? oa : nullptr);
            if (oa[0] > 0) for (int k = 0; k < 3; k++) SetOrigAsp(k, 0.f);
        }
        if (orig > 0 && fabsf(sent - orig) < 1e-4f) SetOrigFov(0.f);   // back to the game's own value
        return;
    }
    if (orig <= 0) { orig = sent; SetOrigFov(orig); LOGI("cull: game FOV %.4f rad (%.1f deg)", orig, orig * 57.2958f); }
    if (g_nAddr == 0) {
        // search for the value the game sends (the original; or our widened one after a core reload)
        if (!g_scanning && GetTickCount() - g_lastScan > 10000) StartScan(sent, nearZ, farZ);
        return;
    }
    float target = 2.f * atanf(tanf(orig * 0.5f) * g_widen);
    if (target > 3.0f) target = 3.0f;   // 172 deg vertical (175 horizontal); the projection breaks down towards 180
    static float lastLogged = -1.f;
    if (fabsf(g_widen - lastLogged) > 1e-3f) {
        lastLogged = g_widen;
        float p00, p11, nz;
        float aspect = Stereo_GameProjection(&p00, &p11, &nz) && p00 > 0 ? p11 / p00 : 1.7778f;
        float h = 2.f * atanf(tanf(target * 0.5f) * aspect) * 57.2958f;
        LOGI("cull: boost %.1f -> game view %.0f deg wide x %.0f deg tall (game default %.0f x %.0f)", g_widen, h, target * 57.2958f,
             2.f * atanf(tanf(orig * 0.5f) * aspect) * 57.2958f, orig * 57.2958f);
    }
    if (g_watchSec > 0 && g_nAddr) {
        DWORD now = GetTickCount();
        if (!g_watchT0) { g_watchT0 = now; g_watchLast = now; for (int i = 0; i < g_nAddr; i++) LOGI("cull: camera copy %d at %p", i, g_fovAddr[i]); Watch_StartAccess(g_fovAddr[0], "game camera FOV"); }
        else if (Watch_Active() && now - g_watchLast >= 2000) {
            g_watchLast = now;
            char lbl[32]; snprintf(lbl, sizeof(lbl), "t=%lus", (now - g_watchT0) / 1000);
            Watch_ReportCounts(lbl);
            if (now - g_watchT0 > (DWORD)g_watchSec * 1000) { Watch_Stop(); g_watchSec = 0; }
        }
    }
    if (OrigAsp(0) == 0.f) { WriteAll(target, nullptr, true); return; }   // capture the original aspects first
    float a[3] = { OrigAsp(0) * g_aspectBoost, OrigAsp(1) * g_aspectBoost, OrigAsp(2) * g_aspectBoost };
    if (g_nAddr) CullSites_Frame((float*)g_fovAddr[0], orig, target);
    WriteAll(CullSites_MemoryWide() ? target : orig, a);
}

void Cull_Shutdown() {
    CullSites_Shutdown();
    float orig = OrigFov();
    float oa[3] = { OrigAsp(0), OrigAsp(1), OrigAsp(2) };
    if (orig > 0 && g_nAddr) WriteAll(orig, oa[0] > 0 ? oa : nullptr);
    // persist[3..6] stay: the next core finds the camera by the (restored) original FOV and re-captures from there
}

} // namespace svr
