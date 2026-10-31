// In-headset settings menu (vrkit, D3D9 backend). Insert opens / closes it. It is drawn into the HUD layer, which the
// host shows world-locked (HudFollowHead = 0), so the menu stays put in space.
// Keys (vrkit): Up/Down select, Left/Right change, Enter toggle/run, Delete reset, PgUp/PgDn page, Backspace close.
// While open, keyboard messages are kept from the game (the game reads the keyboard through window messages).
// Values are written to config.txt when the menu closes; most settings are read when the core loads, so a change
// triggers a core hot reload (about half a second of black in the headset).
#include <windows.h>
#include <d3d9.h>
#include <string>
#include <atomic>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include "core.h"
#include "common/log.h"
#include "common/config.h"
#include "vrkit/vrkit_options.h"
#include "vrkit/vrkit_menu_d3d9.h"

namespace svr {

// Menu copies of the config values (vrkit edits these; they are written back on close).
static struct {
    bool vr = true, pace = true, stereo = true, headTracking = true, shadowReuse = true, followers = true, clipCursor = true;
    float worldScaleDisp = 1.0f, sceneScale = 1.0f, cullWiden = 30.0f, cullAspect = 1.0f, worldScale = 0.1f, sharpen = 0.f, sharpenClamp = 0.035f, sharpenRadius = 1.0f;
    bool hudLayer = true, hudFollowHead = false, drawCursor = true;
    float hudDistance = 1.4f, hudWidth = 1.6f, hudOffsetY = -0.05f;
    int cursorSize = 32;
    float theaterDistance = 2.5f, theaterWidth = 3.2f;
    int theaterAfterMs = 500;
} g_m;

static bool g_inited = false, g_failed = false, g_changed = false, g_wasOpen = false;
static std::atomic<bool> g_open{false};
static HWND g_wnd = nullptr;
static WNDPROC g_prevProc = nullptr;
static IDirect3DStateBlock9* g_sb = nullptr;
static UINT g_menuKey = VK_INSERT;    // MenuKey: open / close the menu
static UINT g_flatKey = VK_F1;
static UINT g_recenterKey = VK_F2;     // RecenterKey: recenter the VR view
static bool g_worldScaleDirty = false;        // FlatToggleKey: VR <-> flat theater screen
static bool g_flatKeyBlock = true;    // FlatToggleKeyToGame = 0: the game does not see that key

static UINT ParseKey(const std::string& s0) {
    std::string s;
    for (char ch : s0) if (ch != ' ') s += (char)toupper((unsigned char)ch);
    if (s.empty() || s == "NONE" || s == "0") return 0;
    if (s[0] == 'F' && s.size() >= 2 && isdigit((unsigned char)s[1])) { int n = atoi(s.c_str() + 1); if (n >= 1 && n <= 24) return VK_F1 + n - 1; }
    if (s.size() > 2 && s[0] == '0' && s[1] == 'X') return (UINT)strtoul(s.c_str() + 2, nullptr, 16);
    if (s.size() == 1 && isalnum((unsigned char)s[0])) return (UINT)s[0];
    if (s == "INS" || s == "INSERT") return VK_INSERT;
    if (s == "DEL" || s == "DELETE") return VK_DELETE;
    if (s == "PAUSE") return VK_PAUSE;
    if (s == "SCROLLLOCK") return VK_SCROLL;
    if (s == "HOME") return VK_HOME;
    if (s == "END") return VK_END;
    return VK_F1;
}

void Menu_LoadSettings(const xrb::Config& c) {
    g_flatKey = ParseKey(c.GetStr("FlatToggleKey", "F1"));
    g_menuKey = ParseKey(c.GetStr("MenuKey", "Insert"));
    g_recenterKey = ParseKey(c.GetStr("RecenterKey", "F2"));
    if (!g_menuKey) g_menuKey = VK_INSERT;
    g_flatKeyBlock = !c.GetBool("FlatToggleKeyToGame", false);
    g_m.vr = c.GetBool("VR", true);
    g_m.stereo = c.GetBool("Stereo", true);
    g_m.pace = c.GetBool("PaceToHeadset", true);
    g_m.headTracking = c.GetBool("HeadTracking", true);
    g_m.shadowReuse = c.GetInt("ShadowReuse", 0) != 0;
    g_m.followers = c.GetBool("SkyFollowsHead", true);
    g_m.sceneScale = c.GetFloat("SceneScale", 1.0f);
    g_m.worldScale = c.GetFloat("WorldScale", 0.1f);
    g_m.worldScaleDisp = g_m.worldScale * 10.f;
    g_m.cullWiden = c.GetFloat("CullWiden", 30.0f);
    g_m.cullAspect = c.GetFloat("CullAspect", 1.0f);
    g_m.sharpen = c.GetFloat("Sharpen", 0.f);
    g_m.sharpenClamp = c.GetFloat("SharpenClamp", 0.035f);
    g_m.sharpenRadius = c.GetFloat("SharpenRadius", 1.0f);
    g_m.hudLayer = c.GetBool("HudLayer", true);
    g_m.hudFollowHead = c.GetBool("HudFollowHead", false);
    g_m.drawCursor = c.GetBool("DrawCursor", true);
    g_m.clipCursor = c.GetBool("ClipCursorToGame", true);
    g_m.hudDistance = c.GetFloat("HudDistance", 1.4f);
    g_m.hudWidth = c.GetFloat("HudWidth", 1.6f);
    g_m.hudOffsetY = c.GetFloat("HudOffsetY", -0.05f);
    g_m.cursorSize = c.GetInt("CursorSize", 32);
    g_m.theaterDistance = c.GetFloat("TheaterDistance", 2.5f);
    g_m.theaterWidth = c.GetFloat("TheaterWidth", 3.2f);
    g_m.theaterAfterMs = c.GetInt("TheaterAfterMs", 500);
}

static void VrkitLog(const char* m) { LOGI("%s", m); }

// Write one "key = value" line into config.txt (replace the existing line or append).
static void WriteConfigKey(const char* key, const char* value) {
    std::wstring path = std::wstring(g_api->modDir) + L"config.txt";
    FILE* f = _wfopen(path.c_str(), L"rb");
    std::string text;
    if (f) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof(b), f)) > 0) text.append(b, n); fclose(f); }
    std::string out; bool done = false; size_t pos = 0; size_t klen = strlen(key);
    while (pos < text.size()) {
        size_t e = text.find('\n', pos); if (e == std::string::npos) e = text.size(); else e++;
        std::string line = text.substr(pos, e - pos);
        size_t s = line.find_first_not_of(" \t");
        if (!done && s != std::string::npos && line.compare(s, klen, key) == 0) {
            size_t q = line.find_first_not_of(" \t", s + klen);
            if (q != std::string::npos && line[q] == '=') { line = std::string(key) + " = " + value + "\r\n"; done = true; }
        }
        out += line; pos = e;
    }
    if (!done) { if (!out.empty() && out.back() != '\n') out += "\r\n"; out += std::string(key) + " = " + value + "\r\n"; }
    f = _wfopen(path.c_str(), L"wb");
    if (f) { fwrite(out.data(), 1, out.size(), f); fclose(f); }
    LOGI("menu: %s = %s saved", key, value);
}

static void Changed() { g_changed = true; }

static void RequestReload() {
    HANDLE ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, L"Local\\SwtorVR_Reload");
    if (ev) { SetEvent(ev); CloseHandle(ev); }
}

// Keep keyboard input from the game while the menu is open; the toggle key never reaches the game.
static LRESULT CALLBACK MenuWndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
        if (g_recenterKey && wp == g_recenterKey && !g_open) {
            if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30))) Vr_Recenter();
            return 0;
        }
        if (g_flatKey && wp == g_flatKey && !g_open) {
            if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30))) Vr_ToggleFlat();
            if (g_flatKeyBlock) return 0;
            break;
        }
        if (wp == g_menuKey || g_open) return 0;
        break;
    case WM_CHAR: case WM_SYSCHAR: case WM_DEADCHAR:
        if (g_open) return 0;
        break;
    }
    return CallWindowProcW(g_prevProc, w, msg, wp, lp);
}

// Display name of a key (for the menu labels; the keys come from config.txt).
static std::string KeyName(UINT vk) {
    char b[16];
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(b, sizeof(b), "F%u", vk - VK_F1 + 1); return b; }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) { b[0] = (char)vk; b[1] = 0; return b; }
    switch (vk) {
    case VK_INSERT: return "Insert"; case VK_DELETE: return "Delete"; case VK_HOME: return "Home"; case VK_END: return "End";
    case VK_PRIOR: return "Page Up"; case VK_NEXT: return "Page Down"; case VK_PAUSE: return "Pause"; case 0: return "none";
    }
    snprintf(b, sizeof(b), "key 0x%02X", vk);
    return b;
}

static void RegisterOptions() {
    auto& o = vrkit::Options::Get();
    if (!o.All().empty()) return;   // registered by an earlier init of this core
    const char* ap = "Applied when the menu closes (the mod reloads).";
    const std::string kRec = KeyName(g_recenterKey), kFlat = KeyName(g_flatKey), kMenu = KeyName(g_menuKey);
    o.AddAction("Display", ("Recenter view (" + kRec + ")").c_str(), [] { Vr_Recenter(); },
                ("Re-centres the VR view on where you are looking now. Quick: " + kMenu + ", Enter, " + kMenu +
                 ". Key: RecenterKey in config.txt.").c_str());
    o.AddFloat("Display", "SceneScale", "Render scale", &g_m.sceneScale, 1.0f, 3.0f, 0.05f, 2,
               "3D resolution = game window x this. Game must be in Windowed display mode.", Changed);
    o.AddFloat("Display", "CullWiden", "Culling FOV boost", &g_m.cullWiden, 1.0f, 30.0f, 0.5f, 1,
               "Widens the game's own camera FOV so objects beside you are not culled when you turn your head (the VR "
               "view is unchanged). Side effect: name plates / markers drawn by the game move toward the centre. "
               "Off in flat mode. Costs some fps. Applies live.", [] { Cull_SetWiden(g_m.cullWiden); });
    auto applySharpen = [] { Sharpen_Set(g_m.sharpen, g_m.sharpenClamp, g_m.sharpenRadius); };
    o.AddFloat("Image", "Sharpen", "Luma sharpen strength", &g_m.sharpen, 0.0f, 10.0f, 0.05f, 2,
               "ReShade LumaSharpen strength. 0 = off. Applies live.", applySharpen);
    o.AddFloat("Image", "SharpenClamp", "Sharpen clamp", &g_m.sharpenClamp, 0.005f, 1.0f, 0.005f, 3,
               "Limits how much one pixel can change (ReShade default 0.035). Applies live.", applySharpen);
    o.AddFloat("Image", "SharpenRadius", "Sharpen radius", &g_m.sharpenRadius, 0.1f, 6.0f, 0.1f, 1,
               "Blur sample offset (ReShade offset bias, default 1.0). Applies live.", applySharpen);
    o.AddFloat("Display", "", "World scale", &g_m.worldScaleDisp, 0.2f, 10.0f, 0.1f, 1,
               "How big the world feels: 1.0 = normal (config WorldScale = value / 10). Larger = world feels smaller. Applies live.",
               [] { g_m.worldScale = g_m.worldScaleDisp * 0.1f; Stereo_SetWorldScale(g_m.worldScale); g_worldScaleDirty = true; });
    o.AddBool("Display", "HeadTracking", "Head tracking", &g_m.headTracking, ap, Changed);
    o.AddBool("Display", "Stereo", "Stereo 3D", &g_m.stereo,
              ("Off: the game is shown on the theater screen. Quick switch in game: " + kFlat + " toggles VR / flat theater "
               "(e.g. for a broken movie). Keys in config.txt: FlatToggleKey, MenuKey, RecenterKey.").c_str(), Changed);
    o.AddAction("Display", ("Switch VR / flat theater now (" + kFlat + ")").c_str(), [] { Vr_ToggleFlat(); },
                ("Same as the " + kFlat + " key (FlatToggleKey in config.txt; FlatToggleKeyToGame = 1 also sends it to the game).").c_str());
    o.AddBool("Display", "ClipCursorToGame", "Keep mouse in game window", &g_m.clipCursor,
              "While in VR: the game window stays in front of the Windows taskbar and the mouse can't leave it. Alt-Tab releases it.", Changed);
    o.AddBool("Display", "SkyFollowsHead", "Sky fix", &g_m.followers, "Background sky cameras follow head rotation.", Changed);
    o.AddFloat("HUD", "HudDistance", "HUD distance (m)", &g_m.hudDistance, 0.5f, 5.0f, 0.05f, 2, ap, Changed);
    o.AddFloat("HUD", "HudWidth", "HUD width (m)", &g_m.hudWidth, 0.5f, 6.0f, 0.05f, 2, ap, Changed);
    o.AddFloat("HUD", "HudOffsetY", "HUD height offset (m)", &g_m.hudOffsetY, -1.0f, 1.0f, 0.01f, 2, ap, Changed);
    o.AddBool("HUD", "HudFollowHead", "HUD follows head", &g_m.hudFollowHead, ap, Changed);
    o.AddBool("HUD", "DrawCursor", "Draw mouse cursor", &g_m.drawCursor, ap, Changed);
    o.AddInt("HUD", "CursorSize", "Cursor size", &g_m.cursorSize, 8, 128, 4, ap, Changed);
    o.AddFloat("Theater", "TheaterDistance", "Theater distance (m)", &g_m.theaterDistance, 1.0f, 20.0f, 0.1f, 1, ap, Changed);
    o.AddFloat("Theater", "TheaterWidth", "Theater width (m)", &g_m.theaterWidth, 0.5f, 30.0f, 0.1f, 1, ap, Changed);
    o.AddInt("Theater", "TheaterAfterMs", "Theater after (ms without VR)", &g_m.theaterAfterMs, 100, 5000, 100,
             "Loading screens / movies switch to the theater after this long without a VR frame.", Changed);
    o.AddAction("Debug", "Save logs for sending", [] { Log_SaveSnapshot(); },
                "Copies the current logs to swtorvr\\logs\\ (with the time in the name) without stopping the game.");
    o.AddAction("Debug", "Reload mod now", [] { g_changed = true; vrkit::menu9::SetOpen(false); }, "Saves and reloads the mod.");
}

static void Init(IDirect3DDevice9* dev) {
    vrkit::SetLog(VrkitLog);
    RegisterOptions();
    vrkit::menu9::Settings9 s;
    s.menu.title = "SWTOR VR";
    s.menu.texW = 1280; s.menu.texH = 960;
    s.menu.fontPx = 34.f;
    s.menu.fontOversample = 1.f;
    s.menu.configPath = std::wstring(g_api->modDir) + L"config.txt";
    s.menu.input.keyboardToggleVk = (int)g_menuKey;
    s.menu.statusLine = [] { return KeyName(g_menuKey) + ": close"; };
    s.restore = [](IDirect3DDevice9*) {};   // state is restored by our state block
    if (!vrkit::menu9::Init(dev, s)) { g_failed = true; LOGW("menu: init failed"); return; }
    g_wnd = g_api->pp.hDeviceWindow ? g_api->pp.hDeviceWindow : g_api->focusWindow;
    if (g_wnd) g_prevProc = (WNDPROC)SetWindowLongPtrW(g_wnd, GWLP_WNDPROC, (LONG_PTR)MenuWndProc);
    g_inited = true;
    LOGI("menu: ready (menu key 0x%02x, flat/VR toggle key 0x%02x%s)", g_menuKey, g_flatKey, g_flatKeyBlock ? "" : ", also sent to the game");
}

// Every Present, before the HUD is handed to the host. Draws the menu onto `target` (the HUD image, premultiplied)
// when open. Returns true when it drew.
bool Menu_OnPresent(IDirect3DDevice9* dev, IDirect3DSurface9* target, const RECT& rect) {
    if (g_failed) return false;
    if (!g_inited) Init(dev);
    if (!g_inited) return false;
    bool open = vrkit::menu9::Update();
    g_open = open;
    if (g_wasOpen && !open) {
        // closed: vrkit saved config.txt; apply by reloading the core
        if (g_worldScaleDirty) { g_worldScaleDirty = false; char v[32]; snprintf(v, sizeof(v), "%.3f", g_m.worldScale); WriteConfigKey("WorldScale", v); }
        if (g_changed) { LOGI("menu: settings changed - reloading"); g_changed = false; RequestReload(); }
    }
    if (open != g_wasOpen) LOGI("menu: %s", open ? "opened" : "closed");
    g_wasOpen = open;
    if (!open || !target) return false;
    if (!g_sb && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_sb))) g_sb = nullptr;
    if (g_sb) g_sb->Capture();
    IDirect3DSurface9* oldRt = nullptr; IDirect3DSurface9* oldDs = nullptr;
    dev->GetRenderTarget(0, &oldRt);
    dev->GetDepthStencilSurface(&oldDs);
    if (SUCCEEDED(dev->BeginScene())) {
        // centred, 60% of the HUD height
        LONG w = rect.right - rect.left, h = rect.bottom - rect.top;
        LONG mh = h * 6 / 10, mw = mh * 4 / 3;
        RECT r = { rect.left + (w - mw) / 2, rect.top + (h - mh) / 2, rect.left + (w + mw) / 2, rect.top + (h + mh) / 2 };
        vrkit::menu9::Render(dev, target, r);
        dev->EndScene();
    }
    if (oldRt) dev->SetRenderTarget(0, oldRt);
    dev->SetDepthStencilSurface(oldDs);
    if (oldRt) oldRt->Release();
    if (oldDs) oldDs->Release();
    if (g_sb) g_sb->Apply();
    return true;
}

bool Menu_IsOpen() { return g_open; }
bool Menu_WantsFrame() { return g_wasOpen; }

void Menu_OnPreReset() {
    if (g_inited) vrkit::menu9::OnLostDevice();
    if (g_sb) { g_sb->Release(); g_sb = nullptr; }
}

void Menu_Shutdown() {
    if (g_wnd && g_prevProc && (WNDPROC)GetWindowLongPtrW(g_wnd, GWLP_WNDPROC) == MenuWndProc)
        SetWindowLongPtrW(g_wnd, GWLP_WNDPROC, (LONG_PTR)g_prevProc);
    g_prevProc = nullptr;
    if (g_inited) vrkit::menu9::Shutdown();
    if (g_sb) { g_sb->Release(); g_sb = nullptr; }
    g_inited = false;
    g_open = false;
}

} // namespace svr
