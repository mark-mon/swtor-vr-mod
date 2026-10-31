// XrBridgeTest_d3d9.exe - 32-bit D3D9Ex test client for XrBridgeHost.exe.
//
// Exercises exactly the path the Final Fantasy XIII mods will use: a 32-bit process with a D3D9Ex device renders
// both eyes + a HUD into the shared texture ring, and the 64-bit host shows it in the headset.
//
// The scene is drawn with nothing but IDirect3DDevice9::Clear rectangles (no shaders needed): a floor grid,
// eight coloured pillars around you and a white marker straight ahead. If the pillars stay put when you move your
// head and look solid (not doubled), poses, FOV and stereo are all correct end to end.
//
// --auto: scripted run for unattended tests. Starts when VR frames flow (or after 15 s without a host):
//   +6 s save snapshot 1 (normal view), +8 s menu mode on, +12 s snapshot 2 (menu), +14 s menu off,
//   +16 s HUD follows head, +19 s snapshot 3, +22 s exit. Snapshots: test_<n>_atlas.bmp here and
//   host_<n>_left/right/hud|menu.bmp from the host (what actually went into the OpenXR swapchains).
// Keys (desktop window focused): R recenter, H toggle HUD, M toggle menu mode, F toggle HUD follows head,
//                                B toggle blocking GPU wait, Esc quit.
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include <map>
#include "xrbridge/client.h"
#include "xrbridge/ring_d3d9.h"
#include "../src/common/log.h"
#include "../src/common/bmp.h"

using namespace xrb;

static IDirect3D9Ex* g_d3d = nullptr;
static IDirect3DDevice9Ex* g_dev = nullptr;
static HWND g_wnd = nullptr;
static std::wstring g_dir;
static bool g_quit = false, g_hud = true, g_menu = false, g_follow = false, g_blocking = false;
static D3D9Ring g_ring;
static IDirect3DSurface9* g_localAtlas = nullptr;   // used when the host is not connected (desktop-only preview)

struct V3 { float x, y, z; };
struct Pt { V3 p; float size; D3DCOLOR c; };
static std::vector<Pt> g_scene;

static void BuildScene() {
    // floor grid 1.4 m below the recentered head position
    for (float x = -3; x <= 3.01f; x += 0.5f)
        for (float z = -3; z <= 3.01f; z += 0.5f) g_scene.push_back({{x, -1.4f, z}, 0.04f, D3DCOLOR_XRGB(90, 90, 90)});
    // eight pillars at 2.5 m, red one straight ahead
    const D3DCOLOR cols[8] = {D3DCOLOR_XRGB(230, 40, 40), D3DCOLOR_XRGB(230, 150, 40), D3DCOLOR_XRGB(220, 220, 40), D3DCOLOR_XRGB(60, 220, 60),
                              D3DCOLOR_XRGB(40, 220, 220), D3DCOLOR_XRGB(60, 90, 240), D3DCOLOR_XRGB(160, 60, 230), D3DCOLOR_XRGB(230, 60, 160)};
    for (int i = 0; i < 8; ++i) {
        float a = i * 3.14159265f / 4;
        for (float y = -1.4f; y <= 1.2f; y += 0.1f) g_scene.push_back({{2.5f * sinf(a), y, -2.5f * cosf(a)}, 0.09f, cols[i]});
    }
    // white marker ahead at eye height, 1.5 m
    g_scene.push_back({{0, 0, -1.5f}, 0.12f, D3DCOLOR_XRGB(255, 255, 255)});
}

// world point -> pixel in an eye rect. Returns false if behind the eye or off-screen.
static bool Project(const IpcPose& e, const IpcFov& f, const RECT& r, V3 w, float size, RECT* out) {
    // eye-space = conj(q) * (w - p) * q
    float vx = w.x - e.px, vy = w.y - e.py, vz = w.z - e.pz;
    float qx = -e.qx, qy = -e.qy, qz = -e.qz, qw = e.qw;
    // rotate v by quaternion (qx,qy,qz,qw): v' = v + 2w(q x v) + 2 q x (q x v)
    float cx = qy * vz - qz * vy, cy = qz * vx - qx * vz, cz = qx * vy - qy * vx;
    float ex = vx + 2 * (qw * cx + (qy * cz - qz * cy));
    float ey = vy + 2 * (qw * cy + (qz * cx - qx * cz));
    float ez = vz + 2 * (qw * cz + (qx * cy - qy * cx));
    if (ez > -0.1f) return false;
    float tx = ex / -ez, ty = ey / -ez;
    float l = tanf(f.left), rr = tanf(f.right), u = tanf(f.up), d = tanf(f.down);
    float W = (float)(r.right - r.left), H = (float)(r.bottom - r.top);
    float px = r.left + (tx - l) / (rr - l) * W;
    float py = r.top + (u - ty) / (u - d) * H;
    float half = 0.5f * size / -ez * W / (rr - l);
    if (half < 1) half = 1;
    RECT o = {(LONG)(px - half), (LONG)(py - half), (LONG)(px + half), (LONG)(py + half)};
    if (o.left < r.left) o.left = r.left;
    if (o.top < r.top) o.top = r.top;
    if (o.right > r.right) o.right = r.right;
    if (o.bottom > r.bottom) o.bottom = r.bottom;
    if (o.left >= o.right || o.top >= o.bottom) return false;
    *out = o;
    return true;
}

static void Fill(const RECT& r, D3DCOLOR c) {
    D3DRECT d{r.left, r.top, r.right, r.bottom};
    g_dev->Clear(1, &d, D3DCLEAR_TARGET, c, 1, 0);
}

static void DrawEye(const IpcPose& pose, const IpcFov& fov, const RECT& r) {
    Fill(r, D3DCOLOR_XRGB(20, 24, 32));
    std::map<D3DCOLOR, std::vector<D3DRECT>> batches;
    for (auto& p : g_scene) {
        RECT o;
        if (Project(pose, fov, r, p.p, p.size, &o)) batches[p.c].push_back({o.left, o.top, o.right, o.bottom});
    }
    for (auto& b : batches) g_dev->Clear((DWORD)b.second.size(), b.second.data(), D3DCLEAR_TARGET, b.first, 1, 0);
}

static void DrawHud(const RECT& r, float t) {
    LONG w = r.right - r.left, h = r.bottom - r.top;
    if (g_menu) {
        Fill(r, D3DCOLOR_ARGB(255, 16, 32, 64));
        for (int i = 0; i < 5; ++i) {
            RECT row{r.left + w / 6, r.top + h / 6 + i * h / 8, r.right - w / 6, r.top + h / 6 + i * h / 8 + h / 12};
            Fill(row, i == ((int)t % 5) ? D3DCOLOR_ARGB(255, 240, 200, 80) : D3DCOLOR_ARGB(255, 70, 90, 130));
        }
        return;
    }
    Fill(r, D3DCOLOR_ARGB(0, 0, 0, 0));                                   // fully transparent
    LONG b = 4;                                                          // frame so the panel edges are visible
    Fill({r.left, r.top, r.right, r.top + b}, D3DCOLOR_ARGB(255, 255, 255, 255));
    Fill({r.left, r.bottom - b, r.right, r.bottom}, D3DCOLOR_ARGB(255, 255, 255, 255));
    Fill({r.left, r.top, r.left + b, r.bottom}, D3DCOLOR_ARGB(255, 255, 255, 255));
    Fill({r.right - b, r.top, r.right, r.bottom}, D3DCOLOR_ARGB(255, 255, 255, 255));
    // "ATB" style bars bottom-left, animated
    for (int i = 0; i < 3; ++i) {
        float fill = 0.5f + 0.5f * sinf(t * (0.7f + i * 0.4f));
        RECT bg{r.left + w / 20, r.bottom - h / 5 + i * h / 18, r.left + w / 3, r.bottom - h / 5 + i * h / 18 + h / 30};
        Fill(bg, D3DCOLOR_ARGB(128, 0, 0, 0));                            // half-transparent backing
        RECT fg = bg; fg.right = bg.left + (LONG)((bg.right - bg.left) * fill);
        Fill(fg, i == 0 ? D3DCOLOR_ARGB(255, 80, 220, 90) : D3DCOLOR_ARGB(255, 80, 160, 240));
    }
}

static void DumpAtlas(IDirect3DSurface9* src, unsigned n) {
    D3DSURFACE_DESC d; src->GetDesc(&d);
    IDirect3DSurface9* sys = nullptr;
    if (FAILED(g_dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) { LOGE("dump: sysmem surface failed"); return; }
    if (SUCCEEDED(g_dev->GetRenderTargetData(src, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%stest_%u_atlas.bmp", g_dir.c_str(), n);
            bool ok = WriteBmp32(path, (const uint8_t*)lr.pBits, d.Width, d.Height, (unsigned)lr.Pitch, false);
            LOGI("dump: %S %ux%u %s", path, d.Width, d.Height, ok ? "saved" : "FAILED");
            sys->UnlockRect();
        }
    } else LOGE("dump: GetRenderTargetData failed");
    sys->Release();
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { g_quit = true; return 0; }
    if (m == WM_KEYDOWN) {
        switch (w) {
        case VK_ESCAPE: g_quit = true; break;
        case 'R': ClientRequestRecenter(); break;
        case 'H': g_hud = !g_hud; break;
        case 'M': g_menu = !g_menu; break;
        case 'F': g_follow = !g_follow; ClientSetHud(1.6f, 1.6f, -0.05f, g_follow); break;
        case 'B': g_blocking = !g_blocking; break;
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static UINT AdapterForLuid(uint64_t luid) {
    if (!luid) return D3DADAPTER_DEFAULT;
    for (UINT i = 0; i < g_d3d->GetAdapterCount(); ++i) {
        LUID l;
        if (SUCCEEDED(g_d3d->GetAdapterLUID(i, &l)) && l.LowPart == (DWORD)luid && (DWORD)l.HighPart == (DWORD)(luid >> 32)) return i;
    }
    LOGW("headset adapter LUID not found among D3D9 adapters - using default");
    return D3DADAPTER_DEFAULT;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    g_dir = dir;
    LogInit((dir + L"XrBridgeTest_d3d9.log").c_str(), LOG_INFO);
    bool autoMode = wcsstr(GetCommandLineW(), L"--auto") != nullptr;
    if (autoMode) LOGI("auto mode: scripted run");
    float autoStart = -1;   // time the script timeline starts
    int autoStep = 0;
    LOGI("XrBridge D3D9Ex test client (%d-bit)", (int)sizeof(void*) * 8);

    ClientSettings cs;
    cs.instanceName = "XrBridgeTest";
    cs.appName = "XrBridge D3D9 test";
    cs.hostExePath = dir + L"XrBridgeHost.exe";
    cs.hostLogPath = dir + L"XrBridgeHost_XrBridgeTest.log";
    cs.hudPremultiplied = false;
    ClientInit(cs);

    unsigned eyeW = 1440, eyeH = 1584;
    bool vr = ClientWaitRecommendedSize(&eyeW, &eyeH, 60000);
    if (!vr) LOGW("no XR host - desktop preview only (%s)", ClientHostError());

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"XrBridgeTest"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT wr{0, 0, 960, 540};
    AdjustWindowRect(&wr, WS_OVERLAPPEDWINDOW, FALSE);
    g_wnd = CreateWindowW(L"XrBridgeTest", L"XrBridge D3D9 test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                          wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, inst, nullptr);

    if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &g_d3d))) { MessageBoxW(g_wnd, L"Direct3DCreate9Ex failed", L"XrBridge test", MB_OK); return 1; }
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 960; pp.BackBufferHeight = 540; pp.hDeviceWindow = g_wnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    UINT adapter = AdapterForLuid(ClientAdapterLuid());
    HRESULT hr = g_d3d->CreateDeviceEx(adapter, D3DDEVTYPE_HAL, g_wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                       &pp, nullptr, &g_dev);
    if (FAILED(hr)) { LOGE("CreateDeviceEx failed %08lX", hr); MessageBoxW(g_wnd, L"CreateDeviceEx failed", L"XrBridge test", MB_OK); return 1; }
    LOGI("D3D9Ex device on adapter %u", adapter);

    unsigned hudW = 1280, hudH = 720;
    if (vr && !g_ring.Create(g_dev, eyeW, eyeH, hudW, hudH)) LOGE("ring creation failed - desktop preview only");
    g_dev->CreateRenderTarget(eyeW * 2 + hudW, eyeH > hudH ? eyeH : hudH, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_localAtlas, nullptr);
    BuildScene();

    IDirect3DSurface9* bb = nullptr;
    LARGE_INTEGER freq, t0, now;
    QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&t0);
    unsigned frames = 0, vrFrames = 0;
    DWORD lastTitle = GetTickCount();

    while (!g_quit) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (g_quit) break;
        if (!ClientWaitHostFrame(40)) Sleep(vr ? 5 : 11);   // headset pacing when visible
        QueryPerformanceCounter(&now);
        float t = (float)(now.QuadPart - t0.QuadPart) / freq.QuadPart;

        static IpcPose lastPose[2]; static IpcFov lastFov[2]; static int64_t lastT = 0; static bool haveLast = false;
        IpcPose pose[2]; IpcFov fov[2]; int64_t xt = 0;
        if (ClientGetLatestPoses(pose, fov, &xt)) {
            memcpy(lastPose, pose, sizeof(pose)); memcpy(lastFov, fov, sizeof(fov)); lastT = xt; haveLast = true;
        } else if (haveLast && ClientHostConnected()) {   // transient seqlock miss: reuse the last real pose
            memcpy(pose, lastPose, sizeof(pose)); memcpy(fov, lastFov, sizeof(fov)); xt = lastT;
        } else {   // desktop preview: fixed IPD, slow look-around
            float yaw = 0.6f * sinf(t * 0.3f);
            for (int e = 0; e < 2; ++e) {
                pose[e] = {(e ? 0.032f : -0.032f) * cosf(yaw), 0, (e ? -0.032f : 0.032f) * sinf(yaw), 0, sinf(yaw / 2), 0, cosf(yaw / 2)};
                fov[e] = {-0.85f, 0.85f, 0.9f, -0.9f};
            }
        }

        // ---- scripted timeline (--auto)
        unsigned dumpId = 0;
        if (autoMode) {
            if (autoStart < 0 && ((vrFrames > 0 && ClientHostVisible()) || t > 15)) { autoStart = t; LOGI("auto: timeline starts (host visible: %d)", (int)ClientHostVisible()); }
            float a = autoStart < 0 ? -1 : t - autoStart;
            struct Step { float at; int what; } steps[] = {{6, 1}, {8, 2}, {12, 3}, {14, 4}, {16, 5}, {19, 6}, {22, 7}};
            if (autoStart >= 0 && autoStep < 7 && a >= steps[autoStep].at) {
                switch (steps[autoStep].what) {
                case 1: dumpId = 1; break;
                case 2: g_menu = true; LOGI("auto: menu on"); break;
                case 3: dumpId = 2; break;
                case 4: g_menu = false; LOGI("auto: menu off"); break;
                case 5: g_follow = true; ClientSetHud(1.6f, 1.6f, -0.05f, true); LOGI("auto: HUD follows head"); break;
                case 6: dumpId = 3; break;
                case 7: LOGI("auto: done"); g_quit = true; break;
                }
                autoStep++;
            }
        }

        int slot = g_ring.Begin();
        IDirect3DSurface9* target = slot >= 0 ? g_ring.Surface(slot) : g_localAtlas;
        RECT er[2] = {g_ring.Valid() ? g_ring.EyeRect(0) : RECT{0, 0, (LONG)eyeW, (LONG)eyeH},
                      g_ring.Valid() ? g_ring.EyeRect(1) : RECT{(LONG)eyeW, 0, (LONG)eyeW * 2, (LONG)eyeH}};
        RECT hr2 = g_ring.Valid() ? g_ring.HudRect() : RECT{(LONG)eyeW * 2, 0, (LONG)(eyeW * 2 + hudW), (LONG)hudH};

        g_dev->SetRenderTarget(0, target);
        g_dev->SetDepthStencilSurface(nullptr);
        g_dev->BeginScene();
        if (!g_menu) { DrawEye(pose[0], fov[0], er[0]); DrawEye(pose[1], fov[1], er[1]); }
        DrawHud(hr2, t);
        g_dev->EndScene();

        // desktop mirror: left eye (or the menu panel) letterboxed into the window
        if (SUCCEEDED(g_dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) {
            g_dev->ColorFill(bb, nullptr, D3DCOLOR_XRGB(0, 0, 0));
            RECT src = g_menu ? hr2 : er[0];
            float sw = (float)(src.right - src.left), sh = (float)(src.bottom - src.top);
            float sc = fminf(960.0f / sw, 540.0f / sh);
            LONG dw = (LONG)(sw * sc), dh = (LONG)(sh * sc);
            RECT dst{(960 - dw) / 2, (540 - dh) / 2, (960 - dw) / 2 + dw, (540 - dh) / 2 + dh};
            g_dev->StretchRect(target, &src, bb, &dst, D3DTEXF_LINEAR);
            bb->Release();
        }
        if (dumpId) {
            DumpAtlas(target, dumpId);
            ClientRequestDump(dumpId);   // host saves the frame it fetches next (this one, or the one right after)
        }
        if (slot >= 0) {
            uint32_t flags = (g_hud || g_menu ? SLOT_HUD_VISIBLE : 0) | (g_menu ? SLOT_MENU_MODE : 0);
            g_ring.End(slot, pose, fov, xt, flags, g_blocking);
            vrFrames++;
        }
        g_dev->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
        frames++;

        DWORD tick = GetTickCount();
        if (tick - lastTitle >= 1000) {
            wchar_t title[256];
            const char* st = !vr ? "no host (desktop preview)" : ClientHostVisible() ? "VISIBLE" : ClientHostConnected() ? "connected (not visible)" : "lost";
            swprintf(title, 256, L"XrBridge D3D9 test | host: %S | %u fps, %u VR frames/s | eye %ux%u | HUD %s%s%s%s",
                     st, frames, vrFrames, eyeW, eyeH, g_hud ? L"on" : L"off", g_menu ? L" | MENU" : L"",
                     g_follow ? L" | follow" : L"", g_blocking ? L" | blocking" : L"");
            SetWindowTextW(g_wnd, title);
            LOGI("%S", title);
            frames = vrFrames = 0; lastTitle = tick;
            const char* err = ClientHostError();
            if (*err) { LOGE("host error: %s", err); }
        }
    }
    ClientShutdown();
    g_ring.Destroy();
    if (g_localAtlas) g_localAtlas->Release();
    g_dev->Release();
    g_d3d->Release();
    LogFlush();
    return 0;
}
