// VR output through XrBridgeHost.exe (OpenXR in a separate 64-bit process; the game shares D3D9Ex textures).
//
// Two kinds of frames:
//   THEATER (SLOT_MENU_MODE): back buffer -> HUD region, shown by the host on a large world-locked panel. Used when
//            stereo is off or not possible (no camera this frame, loading screens, movies...).
//   STEREO:  stereo.cpp runs DrawScene once per eye and copies each result into the eye regions (Vr_CaptureEye);
//            the slot is submitted at Present with the poses the eyes were rendered with.
//
// Requires D3D9Mode = ex in swtorvr\loader.txt (only an IDirect3DDevice9Ex can create shared textures).
#include <windows.h>
#include <d3d9.h>
#include <string>
#include <algorithm>
#include "core.h"
#include "common/log.h"
#include "common/config.h"
#include "xrbridge/client.h"
#include "xrbridge/ring_d3d9.h"

namespace svr {

static xrb::D3D9Ring g_ring;
static IDirect3DDevice9Ex* g_devEx = nullptr;
static bool g_clientUp = false, g_disabled = false;
static unsigned g_eyeW = 0, g_eyeH = 0, g_hudW = 0, g_hudH = 0, g_recW = 0, g_recH = 0;
static uint64_t g_submitted = 0, g_skipped = 0, g_stereoSubmitted = 0, g_heldFrames = 0;
static VrSettings g_vs;
static int g_stereoSlot = -1;                  // slot reserved for the stereo frame in progress

const VrSettings& Vr_Settings() { return g_vs; }

void Vr_LoadSettings(const xrb::Config& c) {
    g_vs.enabled = c.GetBool("VR", true);
    g_vs.theaterDistance = c.GetFloat("TheaterDistance", 2.5f);
    g_vs.theaterWidth = c.GetFloat("TheaterWidth", 3.2f);
    g_vs.theaterAfterMs = c.GetInt("TheaterAfterMs", 500);
    g_vs.clipCursor = c.GetBool("ClipCursorToGame", true);
    g_vs.paceToHeadset = c.GetBool("PaceToHeadset", true);
    g_vs.hostLogLevel = c.GetInt("HostLogLevel", 2);
    g_vs.debugHostDumpEverySec = c.GetInt("DebugHostDumpEverySec", 0);
    g_vs.eyeScale = c.GetFloat("EyeTextureScale", 1.0f);
}

static bool StartClient() {
    xrb::ClientSettings s;
    s.instanceName = "SWTORVR";
    s.appName = "SWTOR VR";
    s.hostExePath = std::wstring(g_api->modDir) + L"XrBridgeHost.exe";
    s.hostLogPath = std::wstring(g_api->modDir) + L"xrhost.log";
    s.logLevel = g_vs.hostLogLevel;
    s.menuDistance = g_vs.theaterDistance;
    s.menuWidth = g_vs.theaterWidth;
    s.hudPremultiplied = true;                 // hud.cpp produces premultiplied alpha (theater uses MENU mode: opaque)
    s.hudDistance = Hud_Settings().distance;
    s.hudWidth = Hud_Settings().width;
    s.hudOffsetY = Hud_Settings().offsetY;
    s.hudFollowHead = Hud_Settings().followHead;
    if (!xrb::ClientInit(s)) { LOGE("vr: XrBridge client init failed"); return false; }
    LOGI("vr: XrBridge client started (host %ls)", s.hostExePath.c_str());
    return true;
}

void Vr_OnDevice(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS* pp) {
    if (!g_vs.enabled || g_disabled) return;
    if (!g_devEx) {
        if (FAILED(dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&g_devEx)) || !g_devEx) {
            LOGW("vr: the game's device is not D3D9Ex - set D3D9Mode = ex in swtorvr\\loader.txt and restart the game. VR output off.");
            g_disabled = true;
            return;
        }
    }
    GameUiSize(&g_hudW, &g_hudH);
    if (!g_clientUp) g_clientUp = StartClient();
}

// Creates the ring once the host reported the headset's size. Returns false while not ready.
static bool EnsureRing() {
    if (g_ring.Valid()) return true;
    if (!g_clientUp || !g_devEx || g_disabled) return false;
    unsigned w = 0, h = 0;
    if (!xrb::ClientWaitRecommendedSize(&w, &h, 1)) return false;   // poll: host still starting
    g_recW = w; g_recH = h;
    g_eyeW = std::max(64u, (unsigned)(w * g_vs.eyeScale));
    g_eyeH = std::max(64u, (unsigned)(h * g_vs.eyeScale));
    if (!g_ring.Create(g_devEx, g_eyeW, g_eyeH, g_hudW, g_hudH)) { LOGE("vr: ring creation failed - VR off"); g_disabled = true; return false; }
    LOGI("vr: ring ready: eyes %ux%u (headset recommends %ux%u), screen/HUD %ux%u", g_eyeW, g_eyeH, w, h, g_hudW, g_hudH);
    return true;
}

bool Vr_StereoReady() { return EnsureRing() && xrb::ClientHostConnected(); }

// Reserve a slot for a stereo frame. False: none free -> the caller renders mono this frame.
bool Vr_BeginStereoFrame() {
    if (g_stereoSlot >= 0) { g_ring.Cancel(g_stereoSlot); g_stereoSlot = -1; }
    g_stereoSlot = g_ring.Begin();
    if (g_stereoSlot < 0) { g_skipped++; return false; }
    return true;
}

static void MaybeDump();

static bool CopyBackBuffer(IDirect3DDevice9* dev, int slot, const RECT& dst) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    HRESULT hr = dev->StretchRect(bb, nullptr, g_ring.Surface(slot), &dst, D3DTEXF_LINEAR);
    bb->Release();
    if (FAILED(hr)) {
        static int n = 0;
        if (n++ < 5) LOGW("vr: StretchRect back buffer -> atlas failed 0x%08lx", hr);
        return false;
    }
    return true;
}

static bool g_stereoPending = false;           // both eyes captured, submit at the next Present
static ULONGLONG g_lastStereoTick = 0;         // GetTickCount64 of the last stereo submit

// Called by stereo.cpp right after the DrawScene pass of each eye.
void Vr_CaptureEye(int eye) {
    if (g_stereoSlot < 0 || !g_api->device) return;
    if (!Sharpen_CopyBackBuffer(g_api->device, g_ring.Surface(g_stereoSlot), g_ring.EyeRect(eye)) &&
        !CopyBackBuffer(g_api->device, g_stereoSlot, g_ring.EyeRect(eye))) return;
    if (eye == 1) g_stereoPending = true;
}

// Pacing (PaceToHeadset = 1): after each Present wait for the host's next headset frame, so the game renders one frame
// per headset frame (90, or 45 when SteamVR throttles) instead of running free: no frames thrown away, no ring slot
// starvation, poses that match the frame the headset shows. Only while stereo frames are going out; capped wait.
static uint64_t g_pacedWaits = 0, g_pacedTimeouts = 0;
void Vr_SetPacing(bool on) { g_vs.paceToHeadset = on; }
void Vr_OnPresentEnd() {
    if (!g_vs.paceToHeadset || !g_clientUp || Vr_FlatMode()) return;
    if (!g_lastStereoTick || GetTickCount64() - g_lastStereoTick > 200) return;
    int64_t period = xrb::ClientDisplayPeriodNs();
    unsigned ms = period > 0 ? (unsigned)(period / 1000000 * 2 + 2) : 40;
    if (ms > 50) ms = 50;
    if (xrb::ClientWaitHostFrame(ms)) g_pacedWaits++; else g_pacedTimeouts++;
}

// Flat mode (FlatToggleKey, default F1): no stereo / head tracking, the game is shown on the theater screen.
// Kept in LoaderApi::persist[2] so it survives core reloads.
void Vr_Recenter() { if (g_clientUp) xrb::ClientRequestRecenter(); }
bool Vr_FlatMode() { return g_api && g_api->persist[2] != nullptr; }
void Vr_ToggleFlat() {
    g_api->persist[2] = Vr_FlatMode() ? nullptr : (void*)1;
    LOGI("vr: %s", Vr_FlatMode() ? "FLAT mode (theater screen) - press the toggle key again for VR" : "VR mode");
}

// While the headset is in use and the game window is in front (ClipCursorToGame, default on):
// - the game window goes above the Windows taskbar (topmost). A window as tall as the screen otherwise lies under the
//   taskbar, which takes the clicks there although you see the game's hotbar in VR;
// - the mouse is kept inside the game window (on the screen), so it can't reach the desktop or another monitor.
// A narrower clip the game sets itself (camera drag) is left alone. Both are undone when the game loses focus
// (Alt-Tab) or the mod unloads.
static bool g_clipped = false, g_topmost = false;
static HWND g_topWnd = nullptr;
static void SetTop(HWND w, bool on) {
    // asynchronous: the window belongs to the game's main thread, which may be waiting for this one
    SetWindowPos(w, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    g_topmost = on; g_topWnd = on ? w : nullptr;
}
static void UpdateCursorClip() {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (now - last < 250) return;
    last = now;
    HWND w = g_api->pp.hDeviceWindow ? g_api->pp.hDeviceWindow : g_api->focusWindow;
    HWND top = w ? GetAncestor(w, GA_ROOT) : nullptr;
    bool want = g_vs.clipCursor && g_clientUp && top && GetForegroundWindow() == top && !IsIconic(top);
    if (want && !g_topmost) { SetTop(top, true); LOGI("vr: game window kept in front of the taskbar, mouse kept inside it (ClipCursorToGame)"); }
    if (!want && g_topmost && g_topWnd) SetTop(g_topWnd, false);
    RECT cr;
    if (want && GetClientRect(w, &cr) && cr.right > 16 && cr.bottom > 16) {
        POINT tl = { 0, 0 }, br = { cr.right, cr.bottom };
        ClientToScreen(w, &tl); ClientToScreen(w, &br);
        RECT want_r = { tl.x, tl.y, br.x, br.y }, cur;
        MONITORINFO mi = { sizeof(mi) };   // only the part on the screen
        if (GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi)) IntersectRect(&want_r, &want_r, &mi.rcMonitor);
        if (GetClipCursor(&cur) && cur.left >= want_r.left && cur.top >= want_r.top && cur.right <= want_r.right && cur.bottom <= want_r.bottom) {
            g_clipped = true;   // already inside (ours or the game's own)
            return;
        }
        ClipCursor(&want_r);
        g_clipped = true;
    } else if (g_clipped) {
        ClipCursor(nullptr);
        g_clipped = false;
    }
}
void Vr_ReleaseCursorClip() {
    if (g_clipped) { ClipCursor(nullptr); g_clipped = false; }
    if (g_topmost && g_topWnd) SetTop(g_topWnd, false);
}

// Called from OnPresentBegin (before the frame reaches the screen).
void Vr_OnPresent(IDirect3DDevice9* dev) {
    UpdateCursorClip();
    if (!EnsureRing()) return;
    MaybeDump();
    if (Vr_FlatMode()) {
        if (g_stereoSlot >= 0) { g_ring.Cancel(g_stereoSlot); g_stereoSlot = -1; }
        g_stereoPending = false;
        g_lastStereoTick = 0;
    }
    if (g_stereoSlot >= 0) {
        int slot = g_stereoSlot;
        g_stereoSlot = -1;
        if (!g_stereoPending) {
            // armed but no DrawScene ran this frame (Bink movie, loading screen): show the frame in the theater instead
            g_ring.Cancel(slot);
            goto theater;
        }
        g_stereoPending = false;
        uint32_t flags = 0;
        if (Hud_EndCapture(dev) && Hud_Surface()) {
            RECT full = { 0, 0, (LONG)g_hudW, (LONG)g_hudH };
            Menu_OnPresent(dev, Hud_Surface(), full);
            RECT hr = g_ring.HudRect();
            if (SUCCEEDED(dev->StretchRect(Hud_Surface(), nullptr, g_ring.Surface(slot), &hr, D3DTEXF_NONE))) flags |= xrb::SLOT_HUD_VISIBLE;
        } else if (Menu_IsOpen() || Menu_WantsFrame()) {
            // no HUD layer (HudLayer = 0): the menu still needs somewhere to appear -> a transparent HUD region
            RECT hr = g_ring.HudRect();
            if (SUCCEEDED(dev->ColorFill(g_ring.Surface(slot), &hr, D3DCOLOR_ARGB(0, 0, 0, 0))) && Menu_OnPresent(dev, g_ring.Surface(slot), hr))
                flags |= xrb::SLOT_HUD_VISIBLE;
        } else {
            Menu_OnPresent(dev, nullptr, RECT{});   // keep the menu's input going (opens on the next frame)
        }
        g_ring.End(slot, Stereo_Poses(), Stereo_Fovs(), Stereo_PoseTime(), flags);
        g_stereoSubmitted++;
        g_lastStereoTick = GetTickCount64();
        return;
    }
theater:
    // A single frame that missed stereo (no free ring slot because the host still holds them, no fresh pose, ...)
    // must not switch the headset to the theater for one frame (flicker when the game runs below the headset rate):
    // the host keeps showing (and reprojecting) the last stereo frame. Only a longer gap (loading screen, movie)
    // goes to the theater.
    if (g_lastStereoTick && GetTickCount64() - g_lastStereoTick < (ULONGLONG)g_vs.theaterAfterMs) {
        g_heldFrames++;
        Menu_OnPresent(dev, nullptr, RECT{});   // keep the menu's input going
        return;
    }
    // theater frame
    int slot = g_ring.Begin();
    if (slot < 0) { g_skipped++; return; }
    if (!CopyBackBuffer(dev, slot, g_ring.HudRect())) { g_ring.Cancel(slot); return; }
    Hud_DrawCursorOnto(dev, g_ring.Surface(slot), g_ring.HudRect());
    Menu_OnPresent(dev, g_ring.Surface(slot), g_ring.HudRect());
    xrb::IpcPose pose[2] = {};
    xrb::IpcFov fov[2] = {};
    int64_t t = 0;
    xrb::ClientGetLatestPoses(pose, fov, &t);
    g_ring.End(slot, pose, fov, t, xrb::SLOT_MENU_MODE | xrb::SLOT_HUD_VISIBLE);
    g_submitted++;
}

void Vr_OnPreReset() {
    Menu_OnPreReset();
    Sharpen_Release();
    Hud_Release();
    if (g_stereoSlot >= 0) { g_ring.Cancel(g_stereoSlot); g_stereoSlot = -1; }
    if (g_ring.Valid()) g_ring.Destroy();
}
void Vr_OnPostReset(const D3DPRESENT_PARAMETERS*) { GameUiSize(&g_hudW, &g_hudH); }

// Debug: ask the host to save what it shows (host_<n>_*.bmp next to xrhost.log), at most 5 per core load.
void Vr_RequestDump(uint32_t id) {
    if (!g_ring.Valid()) return;
    xrb::ClientRequestDump(id);
    LOGI("vr: requested host dump %u", id);
}
static void MaybeDump() {
    static DWORD lastDump = 0;
    static uint32_t dumps = 0;
    if (g_vs.debugHostDumpEverySec <= 0 || dumps >= 5 || !g_ring.Valid()) return;
    DWORD now = GetTickCount();
    if (!lastDump) { lastDump = now; return; }
    if (now - lastDump < (DWORD)g_vs.debugHostDumpEverySec * 1000) return;
    xrb::ClientRequestDump(++dumps + 100 * g_api->reloadCount);
    extern bool g_logEyesNextFrame;
    g_logEyesNextFrame = true;
    lastDump = now;
    LOGI("vr: requested host dump %u", dumps + 100 * g_api->reloadCount);
}

void Vr_LogStats() {
    if (!g_clientUp) return;
    LOGI("vr: host %s%s, stereo frames %llu, theater frames %llu, held (last stereo kept) %llu, skipped %llu%s%s",
         xrb::ClientHostConnected() ? "connected" : "not connected", xrb::ClientHostVisible() ? " (visible)" : "",
         (unsigned long long)g_stereoSubmitted, (unsigned long long)g_submitted, (unsigned long long)g_heldFrames,
         (unsigned long long)g_skipped, *xrb::ClientHostError() ? ", error: " : "", xrb::ClientHostError());
    if (g_vs.paceToHeadset) LOGI("vr: paced to headset: %llu waits, %llu timeouts (headset period %.2f ms)", (unsigned long long)g_pacedWaits,
                                 (unsigned long long)g_pacedTimeouts, xrb::ClientDisplayPeriodNs() / 1e6);
    g_pacedWaits = g_pacedTimeouts = 0;
    g_submitted = g_skipped = g_stereoSubmitted = g_heldFrames = 0;
}

// Core shutdown (phase 1, on the render thread): release the textures and stop the host.
void Vr_Shutdown() {
    Vr_ReleaseCursorClip();
    Menu_Shutdown();
    Sharpen_Release();
    Hud_Release();
    if (g_stereoSlot >= 0) { g_ring.Cancel(g_stereoSlot); g_stereoSlot = -1; }
    if (g_ring.Valid()) g_ring.Destroy();
    if (g_clientUp) { xrb::ClientShutdown(); xrb::ClientClose(); }
    if (g_devEx) { g_devEx->Release(); g_devEx = nullptr; }
    g_clientUp = false;
}

} // namespace svr
