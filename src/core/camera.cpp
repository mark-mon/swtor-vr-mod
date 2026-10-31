// Camera: hooks RemoteRenderer::CameraNode_SetData (found by export name) to learn the layout of
// CameraNodeProxyData (0x110 bytes, copied by the renderer into its camera node at +0x120), and later to inject
// head tracking / per-eye cameras.
//   void RemoteRenderer::CameraNode_SetData(RenderProxyHandle& node, const CameraNodeProxyData& data)
//   rcx = RemoteRenderer*, rdx = RenderProxyHandle* (node object at [rdx+8] when [rdx+0x10] != 0), r8 = data
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <cmath>
#include "MinHook.h"
#include "core.h"
#include "common/log.h"
#include "vrmath.h"
#include "common/config.h"

namespace svr {

constexpr size_t kCamDataSize = 0x110;
constexpr size_t kNodeDataSize = 0xA8;     // NodeProxyData read up to +0xA2 by the renderer

typedef void(__fastcall* PFN_SetData)(void* rr, void* handle, const void* data);
typedef void(__fastcall* PFN_SetActive)(void* rr, void* handle);
static PFN_SetData o_SetData = nullptr, o_NodeSetData = nullptr;
static PFN_SetActive o_SetActive = nullptr;
static void* g_target = nullptr;

// The RenderProxyHandle passed in is a temporary on the game's stack (same address for every camera); the renderer
// resolves it to the node object (handle+8, valid when handle+0x10 != 0) inside the call. Cameras are therefore
// identified by node pointer, read after the original call.
struct CamSlot {
    void* node = nullptr;
    uint64_t camCalls = 0;
    uint8_t cam[kCamDataSize] = {};
};
static CamSlot g_slots[32];
static int g_nSlots = 0;
static void* volatile g_activeNode = nullptr;
static uint64_t g_activeCalls = 0;
static CamSettings g_cs;

static int g_watchFov = 0;          // DebugWatchGameFov = frames: find the game code that writes the camera FOV
static const void* g_lastActiveData = nullptr;
static int g_sameData = 0, g_watchedFrames = -1;
void Camera_LoadSettings(const xrb::Config& c) {
    g_watchFov = c.GetInt("DebugWatchGameFov", 0);
    g_cs.debugPatchMode = c.GetInt("DebugCamPatch", 0);
    g_cs.debugYawDeg = c.GetFloat("DebugCamYawDeg", 0.f);
}

static void* NodeAfter(void* handle) {
    __try { return *((uint8_t*)handle + 0x10) ? *(void**)((uint8_t*)handle + 8) : nullptr; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static CamSlot* FindSlot(void* node) {
    if (!node) return nullptr;
    for (int i = 0; i < g_nSlots; i++) if (g_slots[i].node == node) return &g_slots[i];
    if (g_nSlots >= 32) return nullptr;
    g_slots[g_nSlots].node = node;
    LOGI("camera: new camera node %p (slot %d)", node, g_nSlots);
    return &g_slots[g_nSlots++];
}

// Camera data layout (CameraNodeProxyData, 0x110 bytes) as decoded so far:
//   +0x00 Mat4 camera world (rotation rows, position row 3)     +0x40 Mat4 projection (RH, row2 col3 = -1)
//   +0x80 Mat4 view*projection (far plane variant)               +0xE4 float3 view translation (-pos * R^T)
//   +0xF8 near, +0xFC/+0x100 far, +0x104 vertical FOV (rad)
static bool IsPerspective(const uint8_t* d) {
    const float* f = (const float*)d;
    return fabsf(f[16 + 11] + 1.0f) < 1e-3f;   // projection m[2][3] == -1
}

// Apply a camera-local rotation to the data in place, keeping the derived fields consistent.
void Camera_ApplyLocal(uint8_t* d, const Mat4& local, int mode) {
    Mat4 W = Mat4::From(d), VP = Mat4::From(d + 0x80);
    Mat4 W2 = Mul(local, W);
    if (mode & 1) W2.To(d);
    if (mode & 2) {
        Mat4 Peff = Mul(W, VP);                   // = P (because VP = inv(W) * P)
        Mul(InverseRigid(W2), Peff).To(d + 0x80);
    }
    if (mode & 4) {
        Mat4 V2 = InverseRigid(W2);
        memcpy(d + 0xE4, &V2.m[3][0], 12);
    }
}

static void DumpFloats(const char* what, const void* data, size_t size) {
    const float* f = (const float*)data;
    char line[1024];
    LOGI("  %s:", what);
    for (int r = 0; r < (int)(size / 4); r += 8) {
        int n = snprintf(line, sizeof(line), "    +%03x:", r * 4);
        for (int c = 0; c < 8 && r + c < (int)(size / 4); c++) {
            uint32_t u;
            memcpy(&u, &f[r + c], 4);
            float v = f[r + c];
            if ((u & 0x7f800000) == 0x7f800000 || (u != 0 && u != 0x80000000 && fabsf(v) < 1e-20f) || fabsf(v) > 1e7f)
                n += snprintf(line + n, sizeof(line) - n, "   0x%08x", u);
            else
                n += snprintf(line + n, sizeof(line) - n, " %10.4f", v);
        }
        LOGI("%s", line);
    }
}

static void* g_lastNodeSeen = nullptr;
static uint8_t g_activeLast[kCamDataSize];
static bool g_haveActiveLast = false;

// The handle is unresolved before the call, so the node is unknown there. The active camera is recognised by its
// projection matching the last data the active node received (same FOV / aspect / near-far), which is stable.
static bool LooksLikeActive(const uint8_t* d) {
    if (!g_haveActiveLast) return false;
    if (memcmp(d + 0x40, g_activeLast + 0x40, 0x40) != 0 || memcmp(d + 0xF0, g_activeLast + 0xF0, 0x18) != 0) return false;
    // same lens: also require a position near the last active one (other cameras with the same lens sit at the origin)
    const float* a = (const float*)(d + 0xE4);
    const float* b = (const float*)(g_activeLast + 0xE4);
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    float r2 = a[0] * a[0] + a[1] * a[1] + a[2] * a[2];
    return r2 > 1.0f && dx * dx + dy * dy + dz * dz < 100.0f;
}

static void __fastcall Hk_SetData(void* rr, void* handle, const void* data) {
    HOOK_GUARD;
    // Which node? Resolve first through a dry read: the handle may already be resolved by the game.
    void* nodeBefore = NodeAfter(handle);
    const void* use = data;
    alignas(16) uint8_t mod[kCamDataSize];
    bool patch = false;
    (void)LooksLikeActive;
    if (patch) {
        memcpy(mod, data, kCamDataSize);
        Camera_ApplyLocal(mod, RotY(g_cs.debugYawDeg * 3.14159265f / 180.f), g_cs.debugPatchMode);
        use = mod;
    }
    if (Trace_Active()) Trace_Event("CameraNode_SetData node %p persp %d fov %.3f pos %.1f %.1f %.1f%s", nodeBefore,
                                    IsPerspective((const uint8_t*)data), ((const float*)data)[0x104 / 4],
                                    ((const float*)data)[12], ((const float*)data)[13], ((const float*)data)[14], patch ? " PATCHED" : "");
    o_SetData(rr, handle, use);
    void* node = NodeAfter(handle);
    if (CamSlot* s = FindSlot(node)) { s->camCalls++; memcpy(s->cam, data, kCamDataSize); }
    if (node && !g_activeNode && IsPerspective((const uint8_t*)data)) {
        // Active camera unknown (core reloaded after the game's last SetActiveCameraNode): the main view camera is
        // the perspective camera whose aspect matches the screen and that is not at the origin (UI/portrait
        // cameras sit at ~0). Adopt it until the game reports one.
        const float* f = (const float*)data;
        float aspect = f[16] != 0 ? f[16 + 5] / f[16] : 0;
        unsigned uw = g_api->pp.BackBufferWidth, uh = g_api->pp.BackBufferHeight;
        GameUiSize(&uw, &uh);
        float sw = (float)uw, sh = (float)uh;
        float pos2 = f[12] * f[12] + f[13] * f[13] + f[14] * f[14];
        if (sh > 0 && fabsf(aspect - sw / sh) < 0.02f && pos2 > 1.0f && fabsf(f[0x104 / 4] - 0.8727f) < 0.9f) {
            g_activeNode = node;
            g_api->persist[0] = node;
            LOGI("camera: adopted %p as the main camera (aspect %.3f, fov %.3f)", node, aspect, f[0x104 / 4]);
        }
    }
    if (node && node != g_activeNode && g_haveActiveLast && IsPerspective((const uint8_t*)data)) {
        // Same field of view and aspect as the main camera (near/far may differ): the sky scene backdrop.
        const float* f = (const float*)data;
        const float* a = (const float*)g_activeLast;
        if (fabsf(f[16] - a[16]) < 1e-3f && fabsf(f[21] - a[21]) < 1e-3f) Stereo_OnFollowerCamera(node, data);
    }
    if (node && node == g_activeNode && IsPerspective((const uint8_t*)data)) {
        Stereo_OnActiveCamera(node, data);
        static int logged = 0;
        if (g_watchFov > 0 && logged < 3) {
            logged++;
            void* fr[12]; USHORT n = RtlCaptureStackBackTrace(0, 12, fr, nullptr);
            char line[900]; int k = snprintf(line, sizeof(line), "camera: active SetData callers:");
            for (USHORT i = 0; i < n; i++) {
                HMODULE m = nullptr; char nm[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fr[i], &m)) {
                    GetModuleFileNameA(m, nm, MAX_PATH); const char* b = strrchr(nm, '\\'); if (b) memmove(nm, b + 1, strlen(b));
                }
                k += snprintf(line + k, sizeof(line) - k, " %s+0x%llx", nm, (unsigned long long)((uint8_t*)fr[i] - (uint8_t*)m));
            }
            LOGI("%s (data %p, rsp ~%p)", line, data, (void*)&n);
        }
        if (g_watchFov > 0) {
            if (g_watchedFrames >= 0) {
                if (++g_watchedFrames == g_watchFov) { Watch_Stop(); g_watchFov = 0; }
            } else if (data == g_lastActiveData) {
                if (++g_sameData == 30) { Watch_StartWrite((uint8_t*)data + 0x100, "game camera data +0x100..0x107 (far, FOV)"); g_watchedFrames = 0; }
            } else { g_sameData = 0; LOGI("camera: active camera data at %p", data); }
            g_lastActiveData = data;
        }
        memcpy(g_activeLast, data, kCamDataSize);
        g_haveActiveLast = true;
    }
    g_lastNodeSeen = node;
}
static void __fastcall Hk_NodeSetData(void* rr, void* handle, const void* data) {
    HOOK_GUARD;
    o_NodeSetData(rr, handle, data);
}
static void __fastcall Hk_SetActive(void* rr, void* handle) {
    HOOK_GUARD;
    o_SetActive(rr, handle);
    void* node = NodeAfter(handle);
    if (Trace_Active()) Trace_Event("SetActiveCameraNode node %p", node);
    if (node != g_activeNode) LOGI("camera: active camera node -> %p", node);
    g_activeNode = node;
    g_api->persist[0] = node;   // survives core reloads
    g_activeCalls++;
    FindSlot(node);
}

void* Camera_ActiveNode() { return g_activeNode; }

static bool HookExport(HMODULE rr, const char* name, void* detour, void** orig) {
    void* t = GetProcAddress(rr, name);
    if (!t) { LOGW("camera: export %s not found", name); return false; }
    if (MH_CreateHook(t, detour, orig) != MH_OK || MH_EnableHook(t) != MH_OK) { LOGW("camera: hook %s failed", name); return false; }
    return true;
}

bool Camera_Install(HMODULE rr) {
    g_activeNode = g_api->persist[0];
    if (g_activeNode) LOGI("camera: active camera node %p (kept from the previous core)", g_activeNode);
    bool ok = HookExport(rr, "?CameraNode_SetData@RemoteRenderer@bwa@@QEAAXAEAVRenderProxyHandle@2@AEBVCameraNodeProxyData@2@@Z",
                         (void*)Hk_SetData, (void**)&o_SetData);
    ok &= HookExport(rr, "?Node_SetData@RemoteRenderer@bwa@@QEAAXAEAVRenderProxyHandle@2@AEBVNodeProxyData@2@@Z",
                     (void*)Hk_NodeSetData, (void**)&o_NodeSetData);
    ok &= HookExport(rr, "?SetActiveCameraNode@RemoteRenderer@bwa@@QEAAXAEAVRenderProxyHandle@2@@Z", (void*)Hk_SetActive,
                     (void**)&o_SetActive);
    LOGI("camera: hooks %s", ok ? "installed" : "partly failed");
    return ok;
}

// Every 10 s: list cameras and dump the active one.
void Camera_Tick() {
    static int dumps = 0;
    LOGI("camera: %d nodes, active %p (SetActive calls %llu), patch mode %d yaw %.1f", g_nSlots, g_activeNode,
         (unsigned long long)g_activeCalls, g_cs.debugPatchMode, g_cs.debugYawDeg);
    for (int i = 0; i < g_nSlots; i++) {
        CamSlot& s = g_slots[i];
        const float* f = (const float*)s.cam;
        LOGI("  slot %d node %p%s calls %llu persp %d fov %.3f aspect %.3f world pos %.3f %.3f %.3f", i, s.node,
             s.node == g_activeNode ? " ACTIVE" : "", (unsigned long long)s.camCalls, IsPerspective(s.cam), f[0x104 / 4],
             f[16 + 5] != 0 ? f[16] / f[16 + 5] : 0.f, f[0xE4 / 4], f[0xE8 / 4], f[0xEC / 4]);
        if (dumps < 4 && s.node == g_activeNode) DumpFloats("active camera data", s.cam, kCamDataSize);
    }
    dumps++;
}

} // namespace svr
