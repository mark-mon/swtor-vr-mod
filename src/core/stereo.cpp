// Native stereo by replaying RemoteRenderer::DrawScene once per eye.
//
//   Game thread (single thread): ... CameraNode_SetData(active camera) ... DrawScene() { render; Present; ... }
//
// DrawScene renders only the 3D scene; the UI (Scaleform via IM_*) is drawn after it and Present happens later in
// NotifyRenderComplete. Hk_DrawScene: for each eye, write that eye's camera into the active camera node (exactly
// what CameraNode_SetData does: copy to node+0x120, set node+0x464), call DrawScene, copy the back buffer into the
// eye's region of the shared atlas. The slot is submitted at Present.
// Eye cameras: eye pose from the headset (OpenXR, camera-local: +Y up, -Z forward, which matches SWTOR's camera
// space) applied on top of the game camera; projections rebuilt with the headset's per-eye FOV.
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstring>
#include "MinHook.h"
#include "core.h"
#include "vrmath.h"
#include "common/log.h"
#include "common/config.h"
#include "xrbridge/client.h"

namespace svr {

constexpr size_t kCamSize = 0x110;
constexpr size_t kNodeCamOffset = 0x120;   // CameraNodeProxyData copy inside the renderer's camera node
constexpr size_t kNodeDirtyOffset = 0x464; // set to 1 by CameraNode_SetData

typedef void(__fastcall* PFN_DrawScene)(void* rr, void* timing);
static PFN_DrawScene o_DrawScene = nullptr;
typedef uintptr_t(__fastcall* PFN_Generic4)(uintptr_t, uintptr_t, uintptr_t, uintptr_t);
static PFN_Generic4 o_NotifyRenderBegin = nullptr;
static bool g_frameArmed = false;            // NotifyRenderBegin wrote the left eye and reserved a slot
static uint8_t g_nrbArg[0x18];               // NotifyRenderBegin's timing argument (copied for a refresh call)
static uintptr_t g_nrbRR = 0;
static void* g_drawSceneTarget = nullptr;

static StereoSettings g_ss;
static int g_pass = 0;                         // 0 = not in a stereo frame, 1 = left pass, 2 = right pass
static uint8_t g_center[kCamSize];             // this frame's game camera (active node)
static void* g_centerNode = nullptr;
static bool g_haveCenter = false;
static xrb::IpcPose g_pose[2];
static xrb::IpcFov g_fov[2];
static int64_t g_poseTime = 0;
static uint64_t g_stereoFrames = 0, g_monoFrames = 0;

void Stereo_LoadSettings(const xrb::Config& c) {
    g_ss.enabled = c.GetBool("Stereo", true);
    g_ss.worldScale = c.GetFloat("WorldScale", 0.1f);
    g_ss.headTracking = c.GetBool("HeadTracking", true);
    g_ss.fieldMask = c.GetInt("CamFieldMask", 7);
    g_ss.setProjection = c.GetBool("HeadsetProjection", true);
    g_ss.writeMode = c.GetInt("StereoWriteMode", 1);
    g_ss.debugSeparation = c.GetFloat("DebugEyeSeparation", 0.f);
    g_ss.debugFakeYawDeg = c.GetFloat("DebugFakeHeadYawDeg", 0.f);
    g_ss.debugFixedYawDeg = c.GetFloat("DebugFixedHeadYawDeg", 0.f);
    g_ss.refreshMode = c.GetInt("StereoRefreshMode", 1);
    g_ss.followers = c.GetBool("SkyFollowsHead", true);
}
const StereoSettings& Stereo_Settings() { return g_ss; }
int Stereo_Pass() { return g_pass; }
const xrb::IpcPose* Stereo_Poses() { return g_pose; }
const xrb::IpcFov* Stereo_Fovs() { return g_fov; }
int64_t Stereo_PoseTime() { return g_poseTime; }

// Called by the camera hook after the game set the active camera for this frame.
static float g_gameP00 = 0, g_gameP11 = 0, g_gameNear = 0, g_gameFov = 0;
float Stereo_GameFov() { return g_gameFov; }
static float g_gameFar = 0;
bool Stereo_GameNearFar(float* n, float* f) { if (g_gameNear <= 0 || g_gameFar <= 0) return false; *n = g_gameNear; *f = g_gameFar; return true; }
void Stereo_SetWorldScale(float w) { if (w > 0.001f) g_ss.worldScale = w; }
bool Stereo_GameProjection(float* p00, float* p11, float* nearZ) {
    if (g_gameP00 <= 0 || g_gameP11 <= 0 || g_gameNear <= 0) return false;
    *p00 = g_gameP00; *p11 = g_gameP11; *nearZ = g_gameNear;
    return true;
}

void Stereo_OnActiveCamera(void* node, const void* data) {
    { const float* f = (const float*)data; g_gameP00 = f[16]; g_gameP11 = f[21]; g_gameNear = f[0xF8 / 4]; g_gameFov = f[0x104 / 4]; g_gameFar = f[0xFC / 4]; }
    memcpy(g_center, data, kCamSize);
    g_centerNode = node;
    g_haveCenter = true;
}

static bool g_earlyPoses = false;
static void BuildEye(int eye, uint8_t* out);

// Write mode 1 ("early"): the renderer picks the camera up one step before it renders with it, so the left eye goes
// into the game's own CameraNode_SetData call. Called by the camera hook BEFORE the original SetData for the active
// camera; returns true and fills `out` if the argument should be replaced.
bool Stereo_FilterActiveCamera(const void* data, uint8_t* out) {
    g_earlyPoses = false;
    if (!g_ss.enabled || g_ss.writeMode != 1 || g_shuttingDown || !Vr_StereoReady()) return false;
    if (!xrb::ClientGetLatestPoses(g_pose, g_fov, &g_poseTime)) return false;
    memcpy(g_center, data, kCamSize);
    g_earlyPoses = true;
    BuildEye(0, out);
    return true;
}

// Replace the x/y terms of a RH projection (row-vector form) with an off-centre frustum for the given tangents.
static void SetFrustumXY(Mat4& p, float tl, float tr, float tu, float td) {
    p.m[0][0] = 2.f / (tr - tl);
    p.m[1][1] = 2.f / (tu - td);
    p.m[2][0] = (tr + tl) / (tr - tl);
    p.m[2][1] = (tu + td) / (tu - td);
    p.m[1][0] = p.m[0][1] = p.m[3][0] = p.m[3][1] = 0.f;
}

// Build the camera data for one eye from the frame's centre camera.
// CameraNodeProxyData (decoded 2026-10-30, build 7.9.1c):
//   +0x00 Mat4 VIEW (world -> camera, row-vector, camera looks down -Z)   +0x40 Mat4 projection (RH)
//   +0x80 Mat4 view * projection (far-plane variant)                       +0xC0 float3 forward, right, up (world)
//   +0xE4 float3 camera position (world)   +0xF8 near  +0xFC/+0x100 far  +0x104 vertical FOV (rad)
static void BuildEyeFrom(const uint8_t* center, int eye, uint8_t* out, bool rotationOnly);
static void BuildEye(int eye, uint8_t* out) { BuildEyeFrom(g_center, eye, out, false); }

// rotationOnly: for "follower" cameras such as the sky scene (distant backdrop): they get the head rotation and the
// headset projection but no eye/head translation (their own position scale is unrelated to the world's).
static void BuildEyeFrom(const uint8_t* center, int eye, uint8_t* out, bool rotationOnly) {
    memcpy(out, center, kCamSize);
    Mat4 V = Mat4::From(center);
    Mat4 P = Mat4::From(center + 0x40);
    Mat4 Wc = InverseRigid(V);                     // camera -> world
    Mat4 P80 = Mul(Wc, Mat4::From(center + 0x80)); // projection implied by the stored view*projection
    const xrb::IpcPose& ps = g_pose[eye];
    float s = g_ss.worldScale;
    Mat4 E;                                        // eye pose in camera space (OpenXR axes = SWTOR camera axes)
    if (g_ss.debugSeparation != 0.f) {
        E = Mat4::Identity();
        E.m[3][0] = (eye ? 0.5f : -0.5f) * g_ss.debugSeparation;
    } else if (g_ss.headTracking) {
        E = FromQuat(ps.qx, ps.qy, ps.qz, ps.qw);
        E.m[3][0] = ps.px * s; E.m[3][1] = ps.py * s; E.m[3][2] = ps.pz * s;
    } else {
        E = Mat4::Identity();                      // eyes side by side only: half the IPD along camera x
        E.m[3][0] = (ps.px - 0.5f * (g_pose[0].px + g_pose[1].px)) * s;
    }
    if (g_ss.debugFakeYawDeg != 0.f) {             // test aid: slow head sweep (+-amplitude, 8 s period)
        float a = g_ss.debugFakeYawDeg * 3.14159265f / 180.f * sinf(GetTickCount() * (6.2831853f / 8000.f));
        E = Mul(E, RotY(a));
    }
    if (g_ss.debugFixedYawDeg != 0.f) E = Mul(E, RotY(g_ss.debugFixedYawDeg * 3.14159265f / 180.f));   // test aid: head turned
    if (rotationOnly) { E.m[3][0] = E.m[3][1] = E.m[3][2] = 0.f; }
    Mat4 We = Mul(E, Wc);                          // eye -> world
    Mat4 Ve = InverseRigid(We);
    if (g_ss.setProjection) {
        const xrb::IpcFov& f = g_fov[eye];
        float tl = tanf(f.left), tr = tanf(f.right), tu = tanf(f.up), td = tanf(f.down);
        SetFrustumXY(P, tl, tr, tu, td);
        SetFrustumXY(P80, tl, tr, tu, td);
        P.To(out + 0x40);
        float vfov = atanf(tu) - atanf(td);
        memcpy(out + 0x104, &vfov, 4);
    }
    if (g_ss.fieldMask & 1) Ve.To(out);
    if (g_ss.fieldMask & 2) Mul(Ve, P80).To(out + 0x80);
    if (g_ss.fieldMask & 4) {
        float basis[9] = { -We.m[2][0], -We.m[2][1], -We.m[2][2], We.m[0][0], We.m[0][1], We.m[0][2], We.m[1][0], We.m[1][1], We.m[1][2] };
        memcpy(out + 0xC0, basis, sizeof(basis));
        memcpy(out + 0xE4, &We.m[3][0], 12);
    }
}

// Follower cameras (same lens as the main camera, e.g. the sky scene): their latest data this frame.
struct Follower { void* node; uint8_t data[kCamSize]; uint32_t frame; };
static Follower g_fol[4];
static uint32_t g_frameNo = 0;

void Stereo_OnFollowerCamera(void* node, const void* data) {
    if (!g_ss.followers) return;
    int free = -1;
    for (int i = 0; i < 4; i++) {
        if (g_fol[i].node == node) { memcpy(g_fol[i].data, data, kCamSize); g_fol[i].frame = g_frameNo; return; }
        if (free < 0 && (!g_fol[i].node || g_frameNo - g_fol[i].frame > 120)) free = i;
    }
    if (free >= 0) {
        g_fol[free].node = node;
        memcpy(g_fol[free].data, data, kCamSize);
        g_fol[free].frame = g_frameNo;
        LOGI("stereo: follower camera %p (same lens as the main camera; gets head rotation only)", node);
    }
}

static void WriteFollowers(int eye) {
    for (auto& f : g_fol) {
        if (!f.node || f.frame != g_frameNo) continue;
        uint8_t d[kCamSize];
        BuildEyeFrom(f.data, eye, d, true);
        __try {
            memcpy((uint8_t*)f.node + kNodeCamOffset, d, kCamSize);
            *((uint8_t*)f.node + kNodeDirtyOffset) = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { f.node = nullptr; }
    }
}

bool g_logEyesNextFrame = false;   // set by vr_out when a host dump is requested

static void LogCam(const char* tag, const uint8_t* d) {
    const float* f = (const float*)d;
    LOGI("eyes: %-6s view r0 %8.4f %8.4f %8.4f | r1 %8.4f %8.4f %8.4f | r2 %8.4f %8.4f %8.4f | t %9.4f %9.4f %9.4f | pos %9.4f %9.4f %9.4f | fwd %7.4f %7.4f %7.4f right %7.4f %7.4f %7.4f",
         tag, f[0], f[1], f[2], f[4], f[5], f[6], f[8], f[9], f[10], f[12], f[13], f[14], f[0xE4 / 4], f[0xE8 / 4], f[0xEC / 4],
         f[0xC0 / 4], f[0xC4 / 4], f[0xC8 / 4], f[0xCC / 4], f[0xD0 / 4], f[0xD4 / 4]);
}

static bool WriteEyeToNode(int eye) {
    if (!g_centerNode) return false;
    uint8_t data[kCamSize];
    BuildEye(eye, data);
    if (g_logEyesNextFrame) {
        if (eye == 0) LogCam("center", g_center);
        LogCam(eye == 0 ? "left" : "right", data);
        if (eye == 1) g_logEyesNextFrame = false;
    }
    __try {
        memcpy((uint8_t*)g_centerNode + kNodeCamOffset, data, kCamSize);
        *((uint8_t*)g_centerNode + kNodeDirtyOffset) = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    WriteFollowers(eye);
    return true;
}

static bool WriteEyeToNode(int eye);

// The renderer builds its view for the next DrawScene from the camera node at some point between the game's
// CameraNode_SetData calls and DrawScene (and again after each DrawScene). NotifyRenderBegin is the last call
// before that, so the left eye goes into the node here.
static uintptr_t __fastcall Hk_NotifyRenderBegin(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
    HOOK_GUARD;
    SceneRes_OnFrameBegin((void*)a);
    g_frameArmed = false;
    if (b) memcpy(g_nrbArg, (const void*)b, sizeof(g_nrbArg));
    g_nrbRR = a;
    if (g_ss.enabled && !g_shuttingDown && g_haveCenter && !Vr_FlatMode() && Vr_StereoReady() &&
        xrb::ClientGetLatestPoses(g_pose, g_fov, &g_poseTime) && Vr_BeginStereoFrame()) {
        g_frameArmed = WriteEyeToNode(0);
    }
    return o_NotifyRenderBegin(a, b, c, d);
}

// Make the renderer pick up the camera node again before the second pass (it reads it in its begin-frame step).
//   1: renderer->vtable[0x28/8] (the begin-frame call NotifyRenderBegin makes; clears the back buffer, which was
//      already copied to the left eye)     2: the whole NotifyRenderBegin again with this frame's timing argument
static void RefreshView(void* rr) {
    __try {
        if (g_ss.refreshMode == 1) {
            void* impl = *(void**)rr;
            if (!impl) return;
            typedef void(__fastcall * PFN)(void*);
            ((PFN)(*(void***)impl)[0x28 / 8])(impl);
        } else if (g_ss.refreshMode == 2) {
            alignas(16) uint8_t arg[sizeof(g_nrbArg)];
            memcpy(arg, g_nrbArg, sizeof(arg));
            o_NotifyRenderBegin((uintptr_t)rr, (uintptr_t)arg, 0, 0);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static int n = 0;
        if (n++ < 3) LOGE("stereo: view refresh (mode %d) raised an exception", g_ss.refreshMode);
    }
}

static void __fastcall Hk_DrawScene(void* rr, void* timing) {
    HOOK_GUARD;
    if (Trace_Active()) Trace_Enter("DrawScene");
    // DrawScene renders only the 3D scene; the UI is drawn after it returns and Present happens in
    // NotifyRenderComplete. Pass 1 renders the eye written in NotifyRenderBegin; the renderer re-reads the node after
    // each DrawScene, so the right eye is written between the passes.
    bool stereo = g_frameArmed && !g_shuttingDown;
    g_frameArmed = false;
    if (!stereo) {
        g_monoFrames++;
        o_DrawScene(rr, timing);
    } else {
        g_stereoFrames++;
        g_pass = 1;
        o_DrawScene(rr, timing);
        if (!g_shuttingDown) {
            Vr_CaptureEye(0);
            g_pass = 2;
            WriteEyeToNode(1);
            RefreshView(rr);
            o_DrawScene(rr, timing);
            if (!g_shuttingDown) Vr_CaptureEye(1);
        }
        g_pass = 0;
        // The back buffer now holds the right eye; the game draws its UI next (captured for the HUD layer).
        if (!g_shuttingDown) Hud_BeginCapture(g_api->device);
    }
    g_haveCenter = false;
    g_frameNo++;
    if (Trace_Active()) Trace_Leave("DrawScene");
}

bool Stereo_Install(HMODULE rr) {
    g_drawSceneTarget = GetProcAddress(rr, "?DrawScene@RemoteRenderer@bwa@@QEAAXAEBUFrameTiming@2@@Z");
    if (!g_drawSceneTarget) {
        // the parameter type in the mangled name is a guess; fall back to a prefix match over the export table
        auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)rr + ((IMAGE_DOS_HEADER*)rr)->e_lfanew);
        auto* ex = (IMAGE_EXPORT_DIRECTORY*)((uint8_t*)rr + nt->OptionalHeader.DataDirectory[0].VirtualAddress);
        auto* names = (DWORD*)((uint8_t*)rr + ex->AddressOfNames);
        auto* ords = (WORD*)((uint8_t*)rr + ex->AddressOfNameOrdinals);
        auto* funcs = (DWORD*)((uint8_t*)rr + ex->AddressOfFunctions);
        for (DWORD i = 0; i < ex->NumberOfNames; i++)
            if (strncmp((const char*)rr + names[i], "?DrawScene@RemoteRenderer@bwa@@", 31) == 0) { g_drawSceneTarget = (uint8_t*)rr + funcs[ords[i]]; break; }
    }
    if (!g_drawSceneTarget) { LOGE("stereo: DrawScene export not found"); return false; }
    if (MH_CreateHook(g_drawSceneTarget, (void*)Hk_DrawScene, (void**)&o_DrawScene) != MH_OK || MH_EnableHook(g_drawSceneTarget) != MH_OK) {
        LOGE("stereo: DrawScene hook failed");
        return false;
    }
    void* nrb = nullptr;
    {
        auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)rr + ((IMAGE_DOS_HEADER*)rr)->e_lfanew);
        auto* ex = (IMAGE_EXPORT_DIRECTORY*)((uint8_t*)rr + nt->OptionalHeader.DataDirectory[0].VirtualAddress);
        auto* names = (DWORD*)((uint8_t*)rr + ex->AddressOfNames);
        auto* ords = (WORD*)((uint8_t*)rr + ex->AddressOfNameOrdinals);
        auto* funcs = (DWORD*)((uint8_t*)rr + ex->AddressOfFunctions);
        for (DWORD i = 0; i < ex->NumberOfNames; i++)
            if (strncmp((const char*)rr + names[i], "?NotifyRenderBegin@RemoteRenderer@bwa@@", 39) == 0) { nrb = (uint8_t*)rr + funcs[ords[i]]; break; }
    }
    if (!nrb || MH_CreateHook(nrb, (void*)Hk_NotifyRenderBegin, (void**)&o_NotifyRenderBegin) != MH_OK || MH_EnableHook(nrb) != MH_OK) {
        LOGE("stereo: NotifyRenderBegin hook failed - stereo off");
        g_ss.enabled = false;
    }
    LOGI("stereo: DrawScene hooked (stereo %s, world scale %.2f, head tracking %d, fields %d, headset projection %d)",
         g_ss.enabled ? "on" : "off", g_ss.worldScale, g_ss.headTracking, g_ss.fieldMask, g_ss.setProjection);
    return true;
}

void Stereo_LogStats() {
    LOGI("stereo: %llu stereo frames, %llu mono frames", (unsigned long long)g_stereoFrames, (unsigned long long)g_monoFrames);
    g_stereoFrames = g_monoFrames = 0;
}

} // namespace svr
