// HUD layer: the game's UI (Scaleform, drawn by swtor.exe through IM_* right after DrawScene and before Present) is
// redirected into a private A8R8G8B8 render target cleared to transparent. At Present the HUD texture is
//   1) copied into the HUD region of the VR atlas (premultiplied alpha) for the host's quad layer, and
//   2) blended back over the back buffer so the monitor still shows the normal game image.
// The mouse cursor (a Windows hardware cursor, invisible in VR) is drawn into the HUD texture first.
//
// Alpha: the game's colour blending is left untouched (changing it breaks some UI, see toolkit notes). Only the alpha
// channel's blend is forced to ONE / INVSRCALPHA while the UI draws, which makes the target premultiplied:
//   colour = sum of the game's blended colour over black, alpha = coverage.
#include <windows.h>
#include <d3d9.h>
#include <cstring>
#include <cwchar>
#include "MinHook.h"
#include "core.h"
#include "common/log.h"
#include "common/config.h"

namespace svr {

static HudSettings g_hs;
static IDirect3DTexture9* g_hudTex = nullptr;
static IDirect3DSurface9* g_hudSurf = nullptr;
static IDirect3DSurface9* g_savedRT = nullptr;
static IDirect3DTexture9* g_cursorTex = nullptr;
static unsigned g_w = 0, g_h = 0;          // HUD (UI) size
static unsigned g_bbW = 0, g_bbH = 0;      // back buffer size (scene)
static bool g_capturing = false, g_haveHud = false;
static uint64_t g_frames = 0;
static IDirect3DSurface9* g_curRT0 = nullptr;      // what the game last bound as RT0 while capturing (after our redirect)
static uint64_t g_rtToBB = 0, g_rtOther = 0, g_drawsOff = 0, g_drawsHud = 0;
static bool g_redirect = true;

typedef HRESULT(STDMETHODCALLTYPE* PFN_SetRS)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetRT)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
static PFN_SetRS o_SetRS = nullptr;

void Hud_LoadSettings(const xrb::Config& c) {
    g_hs.enabled = c.GetBool("HudLayer", true);
    g_hs.distance = c.GetFloat("HudDistance", 1.4f);
    g_hs.width = c.GetFloat("HudWidth", 1.6f);
    g_hs.offsetY = c.GetFloat("HudOffsetY", -0.05f);
    g_hs.followHead = c.GetBool("HudFollowHead", false);
    g_hs.drawCursor = c.GetBool("DrawCursor", true);
    g_hs.cursorSize = c.GetInt("CursorSize", 32);
    g_hs.gamePointer = c.GetBool("GamePointer", true);
    g_hs.alphaMode = c.GetInt("HudAlphaMode", 1);
    g_hs.debugDumpAfterSec = c.GetInt("DebugHudDumpAfterSec", 0);
    g_redirect = c.GetBool("HudRedirectBackBuffer", true);
}
const HudSettings& Hud_Settings() { return g_hs; }
bool Hud_Capturing() { return g_capturing; }
bool Hud_HaveFrame() { return g_haveHud; }
IDirect3DSurface9* Hud_Surface() { return g_hudSurf; }

static void ForceAlpha(IDirect3DDevice9* d) {
    o_SetRS(d, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    o_SetRS(d, D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    o_SetRS(d, D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    o_SetRS(d, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
}

// While capturing the UI, keep the alpha blend forced and make sure alpha is written whenever colour is.
static HRESULT STDMETHODCALLTYPE Hk_SetRS(IDirect3DDevice9* d, D3DRENDERSTATETYPE s, DWORD v) {
    HOOK_GUARD;
    if (!g_capturing || g_hs.alphaMode == 0) return o_SetRS(d, s, v);
    switch (s) {
    case D3DRS_SEPARATEALPHABLENDENABLE: case D3DRS_SRCBLENDALPHA: case D3DRS_DESTBLENDALPHA: case D3DRS_BLENDOPALPHA:
        return D3D_OK;   // ours win
    case D3DRS_COLORWRITEENABLE:
        if (v & 7) v |= D3DCOLORWRITEENABLE_ALPHA;
        return o_SetRS(d, s, v);
    default:
        return o_SetRS(d, s, v);
    }
}

bool Hud_Install(IDirect3DDevice9* dev) {
    void** vt = *(void***)dev;
    if (MH_CreateHook(vt[57], (void*)Hk_SetRS, (void**)&o_SetRS) != MH_OK || MH_EnableHook(vt[57]) != MH_OK) {
        LOGW("hud: SetRenderState hook failed - HUD layer off");
        g_hs.enabled = false;
        return false;
    }
    return true;
}

static bool EnsureTex(IDirect3DDevice9* dev) {
    D3DSURFACE_DESC bd;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
    bb->GetDesc(&bd);
    bb->Release();
    g_bbW = bd.Width; g_bbH = bd.Height;
    unsigned uw, uh;
    GameUiSize(&uw, &uh);                     // the UI's own size (smaller than the back buffer with SceneScale)
    if (g_hudTex && g_w == uw && g_h == uh) return true;
    Hud_Release();
    g_bbW = bd.Width; g_bbH = bd.Height;
    if (FAILED(dev->CreateTexture(uw, uh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_hudTex, nullptr))) {
        LOGE("hud: texture %ux%u failed", uw, uh);
        return false;
    }
    g_hudTex->GetSurfaceLevel(0, &g_hudSurf);
    g_w = uw; g_h = uh;
    LOGI("hud: capture target %ux%u (back buffer %ux%u)", g_w, g_h, g_bbW, g_bbH);
    return true;
}

// End of the stereo DrawScene: the UI is drawn next. Redirect it into the HUD target.
void Hud_BeginCapture(IDirect3DDevice9* dev) {
    if (!g_hs.enabled || !o_SetRS || g_capturing) return;
    if (!EnsureTex(dev)) return;
    if (FAILED(dev->GetRenderTarget(0, &g_savedRT))) { g_savedRT = nullptr; return; }
    dev->SetRenderTarget(0, g_hudSurf);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.f, 0);
    g_curRT0 = g_hudSurf;
    g_capturing = true;
    if (g_hs.alphaMode) ForceAlpha(dev);
}

// Simple screen-space quad drawing (fixed function), used for the cursor and for compositing the HUD back.
struct QV { float x, y, z, rhw; DWORD c; float u, v; };
static void DrawQuad(IDirect3DDevice9* dev, IDirect3DBaseTexture9* tex, float x0, float y0, float x1, float y1, DWORD color, bool premul) {
    QV q[4] = { { x0 - .5f, y0 - .5f, 0, 1, color, 0, 0 }, { x1 - .5f, y0 - .5f, 0, 1, color, 1, 0 },
                { x0 - .5f, y1 - .5f, 0, 1, color, 0, 1 }, { x1 - .5f, y1 - .5f, 0, 1, color, 1, 1 } };
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->SetTexture(0, tex);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, premul ? D3DBLEND_ONE : D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QV));
}

// A small arrow cursor (premultiplied A8R8G8B8, 32x32), built once.
// The pointer image: the game's own pointer (whatever Windows currently shows: SWTOR switches pointers for talk,
// loot, taxi, ...), converted once per pointer handle; our drawn arrow when that fails.
static HCURSOR g_curHandle = nullptr;
static int g_curW = 32, g_curH = 32, g_hotX = 0, g_hotY = 0;
static bool g_curFallback = false;

static bool BuildArrow(IDirect3DDevice9* dev) {
    if (FAILED(dev->CreateTexture(32, 32, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_cursorTex, nullptr))) return false;
    D3DLOCKED_RECT lr;
    if (FAILED(g_cursorTex->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) { g_cursorTex->Release(); g_cursorTex = nullptr; return false; }
    for (int y = 0; y < 32; y++) {
        DWORD* row = (DWORD*)((uint8_t*)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < 32; x++) {
            bool inside = y < 26 && x <= y * 0.62f && !(y > 18 && x > 8 && x < 14 - (y - 18) / 2);
            bool edge = inside && (x == 0 || x >= (int)(y * 0.62f) - 1 || y >= 25);
            row[x] = !inside ? 0 : edge ? 0xFF000000 : 0xFFFFFFFF;
        }
    }
    g_cursorTex->UnlockRect(0);
    g_curW = g_curH = 32; g_hotX = g_hotY = 0;
    return true;
}

// Render the pointer over black and over white: alpha = 1 - (white - black), colour = the black render (already
// premultiplied). Works for colour pointers with alpha and for old monochrome ones.
static bool BuildFromHandle(IDirect3DDevice9* dev, HCURSOR hc) {
    ICONINFO ii = {};
    if (!GetIconInfo(hc, &ii)) return false;
    BITMAP bm = {};
    int w = 32, h = 32;
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm)) { w = bm.bmWidth; h = bm.bmHeight; }
    else if (ii.hbmMask && GetObjectW(ii.hbmMask, sizeof(bm), &bm)) { w = bm.bmWidth; h = bm.bmHeight / 2; }
    int hx = (int)ii.xHotspot, hy = (int)ii.yHotspot;
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (w <= 0 || h <= 0 || w > 256 || h > 256) return false;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    HDC dc = CreateCompatibleDC(nullptr);
    void* pb = nullptr; void* pw = nullptr;
    HBITMAP bb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pb, nullptr, 0);
    HBITMAP bw = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &pw, nullptr, 0);
    bool ok = false;
    if (dc && bb && bw && pb && pw) {
        memset(pb, 0x00, (size_t)w * h * 4);
        memset(pw, 0xFF, (size_t)w * h * 4);
        HGDIOBJ old = SelectObject(dc, bb);
        DrawIconEx(dc, 0, 0, hc, w, h, 0, nullptr, DI_NORMAL);
        SelectObject(dc, bw);
        DrawIconEx(dc, 0, 0, hc, w, h, 0, nullptr, DI_NORMAL);
        SelectObject(dc, old);
        GdiFlush();
        IDirect3DTexture9* t = nullptr;
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_DYNAMIC, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, nullptr)) &&
            SUCCEEDED(t->LockRect(0, &lr, nullptr, D3DLOCK_DISCARD))) {
            const uint8_t* B = (const uint8_t*)pb; const uint8_t* W = (const uint8_t*)pw;
            int any = 0;
            for (int y = 0; y < h; y++) {
                DWORD* row = (DWORD*)((uint8_t*)lr.pBits + y * lr.Pitch);
                for (int x = 0; x < w; x++) {
                    const uint8_t* k = B + ((size_t)y * w + x) * 4; const uint8_t* q = W + ((size_t)y * w + x) * 4;
                    int d = ((q[0] - k[0]) + (q[1] - k[1]) + (q[2] - k[2])) / 3;
                    int al = 255 - (d < 0 ? 0 : d > 255 ? 255 : d);
                    int r = k[2] < al ? k[2] : al, g = k[1] < al ? k[1] : al, b = k[0] < al ? k[0] : al;   // premultiplied
                    row[x] = ((DWORD)al << 24) | (r << 16) | (g << 8) | b;
                    any |= al;
                }
            }
            t->UnlockRect(0);
            if (any) {
                if (g_cursorTex) g_cursorTex->Release();
                g_cursorTex = t; t = nullptr;
                g_curW = w; g_curH = h; g_hotX = hx; g_hotY = hy;
                ok = true;
            }
        }
        if (t) t->Release();
    }
    if (bb) DeleteObject(bb);
    if (bw) DeleteObject(bw);
    if (dc) DeleteDC(dc);
    return ok;
}

static bool EnsureCursor(IDirect3DDevice9* dev, HCURSOR hc) {
    if (g_hs.gamePointer && hc && hc != g_curHandle) {
        g_curHandle = hc;
        if (BuildFromHandle(dev, hc)) { g_curFallback = false; return true; }
        static int n = 0;
        if (n++ < 3) LOGW("hud: could not convert the game pointer %p - drawing an arrow", hc);
        if (g_cursorTex && !g_curFallback) { g_cursorTex->Release(); g_cursorTex = nullptr; }
    }
    if (!g_cursorTex) { g_curFallback = true; return BuildArrow(dev); }
    return true;
}

// Before Present: stop capturing, draw the cursor into the HUD, put the HUD over the back buffer for the monitor.
// Returns true if this frame has a HUD image (Hud_Surface) for the VR atlas.
// The mouse pointer at its position within `r` of the current render target (the game's own pointer is drawn by
// Windows on top of the monitor image, so it is never in the frames sent to the headset).
static void DrawCursorAt(IDirect3DDevice9* dev, const RECT& r) {
    POINT p;
    HWND w = g_api->pp.hDeviceWindow ? g_api->pp.hDeviceWindow : g_api->focusWindow;
    CURSORINFO ci = { sizeof(ci) };
    if (!GetCursorPos(&p) || !ScreenToClient(w, &p) || !GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) return;
    if (!EnsureCursor(dev, ci.hCursor)) return;
    RECT cr;
    GetClientRect(w, &cr);
    float rw = (float)(r.right - r.left), rh = (float)(r.bottom - r.top);
    float sx = cr.right > 0 ? rw / cr.right : 1.f, sy = cr.bottom > 0 ? rh / cr.bottom : 1.f;
    float x = r.left + p.x * sx, y = r.top + p.y * sy;
    if (x < r.left || y < r.top || x >= r.right || y >= r.bottom) return;
    // CursorSize 32 = the pointer's own size (in UI pixels); the hotspot stays on the mouse position
    float k = (float)g_hs.cursorSize / 32.f * (g_w > 0 ? rw / g_w : 1.f);
    x -= g_hotX * k; y -= g_hotY * k;
    DrawQuad(dev, g_cursorTex, x, y, x + g_curW * k, y + g_curH * k, 0xFFFFFFFF, true);
}

// Flat / theater frames: draw the pointer into `rect` of `target` (the frame going to the headset).
void Hud_DrawCursorOnto(IDirect3DDevice9* dev, IDirect3DSurface9* target, const RECT& rect) {
    if (!g_hs.drawCursor || !target) return;
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) sb = nullptr;
    IDirect3DSurface9* oldRt = nullptr; IDirect3DSurface9* oldDs = nullptr;
    dev->GetRenderTarget(0, &oldRt);
    dev->GetDepthStencilSurface(&oldDs);
    IDirect3DSurface9* oldMrt[3] = {};
    for (DWORD i = 1; i < 4; i++) { dev->GetRenderTarget(i, &oldMrt[i - 1]); if (oldMrt[i - 1]) dev->SetRenderTarget(i, nullptr); }
    dev->SetRenderTarget(0, target);
    dev->SetDepthStencilSurface(nullptr);
    DrawCursorAt(dev, rect);
    if (oldRt) dev->SetRenderTarget(0, oldRt);
    for (DWORD i = 1; i < 4; i++) if (oldMrt[i - 1]) { dev->SetRenderTarget(i, oldMrt[i - 1]); oldMrt[i - 1]->Release(); }
    dev->SetDepthStencilSurface(oldDs);
    if (oldRt) oldRt->Release();
    if (oldDs) oldDs->Release();
    if (sb) { sb->Apply(); sb->Release(); }
}

// The UI pass may switch render targets (Scaleform filters / cached panels drawn into offscreen textures) and then
// bind the back buffer again to carry on: without this those later UI draws would land in the back buffer (already
// copied to the eyes), so they show on the monitor but not in the headset - and which panels that hits changes from
// frame to frame. While capturing, the back buffer is swapped for the HUD surface.
IDirect3DSurface9* Hud_FilterRT(DWORD index, IDirect3DSurface9* s) {
    if (!g_capturing || index != 0) return s;
    if (s && s == g_savedRT && g_redirect) { g_rtToBB++; g_curRT0 = g_hudSurf; return g_hudSurf; }
    if (s != g_hudSurf) g_rtOther++;
    g_curRT0 = s;
    return s;
}
void Hud_OnDraw() {
    if (!g_capturing) return;
    if (g_curRT0 == g_hudSurf) g_drawsHud++; else g_drawsOff++;
}

bool Hud_EndCapture(IDirect3DDevice9* dev) {
    g_haveHud = false;
    if (!g_capturing) return false;
    g_capturing = false;
    IDirect3DStateBlock9* sb = nullptr;
    dev->CreateStateBlock(D3DSBT_ALL, &sb);
    if (g_hs.drawCursor) {
        RECT r = { 0, 0, (LONG)g_w, (LONG)g_h };
        dev->SetRenderTarget(0, g_hudSurf);
        DrawCursorAt(dev, r);
    }
    if (g_savedRT) {
        dev->SetRenderTarget(0, g_savedRT);
        DrawQuad(dev, g_hudTex, 0, 0, (float)g_bbW, (float)g_bbH, 0xFFFFFFFF, true);   // HUD over the monitor image
        g_savedRT->Release();
        g_savedRT = nullptr;
    }
    if (sb) { sb->Apply(); sb->Release(); }
    // Debug: save the HUD (with alpha) once, via the game's own d3dx9_43.dll.
    static DWORD t0 = GetTickCount();
    static bool dumped = false;
    if (g_hs.debugDumpAfterSec > 0 && !dumped && GetTickCount() - t0 > (DWORD)g_hs.debugDumpAfterSec * 1000) {
        dumped = true;
        typedef HRESULT(WINAPI * PFN_Save)(LPCWSTR, DWORD, IDirect3DSurface9*, const PALETTEENTRY*, const RECT*);
        HMODULE dx = GetModuleHandleW(L"d3dx9_43.dll");
        auto save = dx ? (PFN_Save)GetProcAddress(dx, "D3DXSaveSurfaceToFileW") : nullptr;
        if (save) {
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%shud_dump_%u.png", g_api->modDir, g_api->reloadCount);
            HRESULT hr = save(path, 3 /*D3DXIFF_PNG*/, g_hudSurf, nullptr, nullptr);
            LOGI("hud: dump %ls -> 0x%08lx", path, hr);
        }
    }
    g_haveHud = true;
    g_frames++;
    return true;
}

void Hud_Release() {
    if (g_savedRT) {
        bool was = g_capturing;
        g_capturing = false;   // before SetRenderTarget: the redirect must not apply to our own restore
        if (was && g_api && g_api->device) g_api->device->SetRenderTarget(0, g_savedRT);
        g_savedRT->Release();
        g_savedRT = nullptr;
    }
    if (g_hudSurf) { g_hudSurf->Release(); g_hudSurf = nullptr; }
    if (g_hudTex) { g_hudTex->Release(); g_hudTex = nullptr; }
    if (g_cursorTex) { g_cursorTex->Release(); g_cursorTex = nullptr; }
    g_curHandle = nullptr;
    g_capturing = false;
    g_w = g_h = 0;
}

void Hud_LogStats() {
    LOGI("hud: %llu captured frames (layer %s, %ux%u); UI draws %llu to HUD, %llu elsewhere; back buffer re-bound %llu x (%s), other targets %llu x",
         (unsigned long long)g_frames, g_hs.enabled ? "on" : "off", g_w, g_h, (unsigned long long)g_drawsHud, (unsigned long long)g_drawsOff,
         (unsigned long long)g_rtToBB, g_redirect ? "redirected" : "not redirected", (unsigned long long)g_rtOther);
    g_drawsHud = g_drawsOff = g_rtToBB = g_rtOther = 0;
    g_frames = 0;
}

} // namespace svr
