// Scene resolution: make only the RENDERER believe the game window is larger, so the 3D scene renders at
// window size x SceneScale while swtor.exe (which lays out the UI) keeps the real window size.
//
// RemoteRenderer.dll owns the game window and reads its size with user32!GetClientRect (its own import). We patch
// that one IAT entry (only in RemoteRenderer.dll) and then send the window a WM_SIZE so the renderer re-reads the
// size and resets the device at the scaled size. The UI is captured into a HUD texture at the real window size
// (hud.cpp), so the HUD layout is unchanged. Removing the patch (core unload / SceneScale = 1) resizes back.
#include <windows.h>
#include <cstring>
#include "core.h"
#include "common/log.h"
#include "common/config.h"

namespace svr {

typedef BOOL(WINAPI* PFN_GetClientRect)(HWND, LPRECT);
static PFN_GetClientRect o_GetClientRect = nullptr;
static void** g_iatSlot = nullptr;
static float g_scale = 1.0f;
static HWND g_wnd = nullptr;
static volatile bool g_pendingResize = false;
static volatile ULONGLONG g_lieUntil = 0;
static ULONGLONG g_askedAt = 0;            // when we last asked the game to resize; checked 3 s later
static int g_askTries = 0;
static bool g_resetSinceAsk = false;      // the game did reset the device after we asked
static bool g_gaveUp = false;             // the game resets but ignores the scaled size (fullscreen windowed): stop asking
static LONG g_gaveUpW = 0, g_gaveUpH = 0;
static LONG g_maxDim = 8192;              // largest back buffer side (device caps)  // until then swtor.exe sees a slightly different window size (GetTickCount64)
static bool g_direct = false;              // SceneResDirect=1: call RemoteRenderer::HandleDeviceReset ourselves
static void** g_gameIatSlot = nullptr;
static int g_resetSlot = -1;          // renderer vtable slot that re-reads the window size and resets the device
static int g_cfgSlot = -1;

// swtor.exe keeps the renderer's size (reported through the device-reset callback registered with
// RemoteRendererClientDLL::registerOnDeviceResetCallback) and compares it with its own GetClientRect every frame; on a
// mismatch it asks the renderer to reset again (endless reset loop) and it also lays out the UI for the reported size.
// So the callback is wrapped to report the REAL window size. The callback pointer lives in RemoteRendererClient.dll; the
// export is "mov [rip+disp32], rcx; ret", which gives its address.
static BOOL WINAPI Hk_GameGetClientRect(HWND w, LPRECT r);
typedef void(__cdecl* PFN_ResetCb)(unsigned type, unsigned w, unsigned h);
static PFN_ResetCb* g_cbSlot = nullptr;
static PFN_ResetCb o_ResetCb = nullptr;
static void __cdecl Hk_ResetCb(unsigned type, unsigned w, unsigned h) {
    HOOK_GUARD;
    unsigned rw, rh;
    if (type == 0 && g_wnd && o_GetClientRect) {
        RECT r;   // exactly what swtor.exe's GetClientRect returns now, so its size comparison matches
        if (Hk_GameGetClientRect(g_wnd, &r) && r.right > 0 && r.bottom > 0) { rw = r.right; rh = r.bottom; w = rw; h = rh; }
    }
    o_ResetCb(type, w, h);
}
static void WrapResetCallback() {
    HMODULE cl = GetModuleHandleW(L"RemoteRendererClient.dll");
    const uint8_t* f = cl ? (const uint8_t*)GetProcAddress(cl, "?registerOnDeviceResetCallback@RemoteRendererClientDLL@bwa@@SAXP6AXIII@Z@Z") : nullptr;
    if (!f || f[0] != 0x48 || f[1] != 0x89 || f[2] != 0x0D) { LOGW("sceneres: device reset callback not found - the game may reset in a loop"); return; }
    g_cbSlot = (PFN_ResetCb*)(f + 7 + *(const int32_t*)(f + 3));
    if (!*g_cbSlot || *g_cbSlot == Hk_ResetCb) { g_cbSlot = nullptr; return; }
    o_ResetCb = *g_cbSlot;
    *g_cbSlot = Hk_ResetCb;   // plain data in .data, writable
    LOGI("sceneres: device reset callback wrapped (game sees the real window size)");
}
static void UnwrapResetCallback() {
    if (g_cbSlot && *g_cbSlot == Hk_ResetCb) *g_cbSlot = o_ResetCb;
    g_cbSlot = nullptr;
}

void SceneRes_LoadSettings(const xrb::Config& c) {
    g_scale = c.GetFloat("SceneScale", 1.0f);
    if (g_scale < 1.0f) g_scale = 1.0f;
    if (g_scale > 4.0f) g_scale = 4.0f;
    g_cfgSlot = c.GetInt("SceneResResetSlot", -1);
    g_direct = c.GetInt("SceneResDirect", 0) != 0;
}

static BOOL WINAPI Hk_GetClientRect(HWND w, LPRECT r) {
    HOOK_GUARD;
    BOOL ok = o_GetClientRect(w, r);
    if (ok && r && w == g_wnd && g_scale > 1.001f) {
        LONG cw = r->right - r->left, ch = r->bottom - r->top;
        float k = g_scale;   // keep within what the GPU can allocate (same factor on both sides)
        if (cw > 0 && cw * k > g_maxDim) k = (float)g_maxDim / cw;
        if (ch > 0 && ch * k > g_maxDim) k = (float)g_maxDim / ch;
        if (k < 1.f) k = 1.f;
        r->right = r->left + ((LONG)(cw * k + 0.5f) & ~1L);
        r->bottom = r->top + ((LONG)(ch * k + 0.5f) & ~1L);
    }
    return ok;
}

// swtor.exe's own GetClientRect: normally untouched. To make the game run its normal "window was resized" path
// (prepare UI, RemoteRenderer::HandleDeviceReset, device-reset callback) we let it see a 2-pixel narrower window for a
// couple of frames; it then sees the real size again and resets the renderer, which reads the scaled size.
static BOOL WINAPI Hk_GameGetClientRect(HWND w, LPRECT r) {
    HOOK_GUARD;
    BOOL ok = o_GetClientRect(w, r);
    if (ok && r && w == g_wnd && g_lieUntil && GetTickCount64() < g_lieUntil && r->right - r->left > 64) r->right -= 2;
    return ok;
}

// The real (unscaled) client size of the game window = the size the UI is laid out for.
bool SceneRes_RealClientSize(unsigned* w, unsigned* h) {
    if (!g_wnd) return false;
    RECT r;
    PFN_GetClientRect f = o_GetClientRect ? o_GetClientRect : GetClientRect;
    if (!f(g_wnd, &r) || r.right <= 0 || r.bottom <= 0) return false;
    *w = r.right; *h = r.bottom;
    return true;
}

static void** FindIat(HMODULE mod, const char* dll, const char* func) {
    auto* base = (uint8_t*)mod;
    auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
        if (_stricmp((const char*)base + imp->Name, dll) != 0) continue;
        auto* names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto* addrs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, func) == 0) return (void**)&addrs->u1.Function;
        }
    }
    return nullptr;
}

static void WriteSlot(void** slot, void* v) {
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    *slot = v;
    VirtualProtect(slot, sizeof(void*), old, &old);
}

// The renderer re-reads the window size only in its device "resize/reset" method (renderer impl vtable slot 15 in
// the current build: builds present parameters from GetClientRect and resets the device). WM_SIZE alone does not
// trigger it. The slot is found by scanning the impl vtable for the method that calls GetClientRect through the
// import we patched and starts by testing the device pointer (cmp qword [rcx+disp32], 0) - slot 1 (init) also
// calls GetClientRect but takes arguments. Config SceneResResetSlot overrides.
static bool CallsIat(const uint8_t* f, size_t len) {
    __try {
        for (size_t i = 0; i + 6 <= len; i++)
            if (f[i] == 0xFF && f[i + 1] == 0x15) {
                const uint8_t* t = f + i + 6 + *(const int32_t*)(f + i + 2);
                if ((void**)t == g_iatSlot) return true;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
static bool StartsWithDeviceTest(const uint8_t* f) {
    __try {
        for (int i = 0; i < 24; i++)
            if (f[i] == 0x48 && f[i + 1] == 0x83 && f[i + 2] == 0xB9 && f[i + 7] == 0x00) return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
static int FindResetSlot(void* impl) {
    HMODULE rr = GetModuleHandleW(L"RemoteRenderer.dll");
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)rr + ((IMAGE_DOS_HEADER*)rr)->e_lfanew);
    uint8_t* lo = (uint8_t*)rr; uint8_t* hi = lo + nt->OptionalHeader.SizeOfImage;
    void** vt = *(void***)impl;
    if ((uint8_t*)vt < lo || (uint8_t*)vt >= hi) return -1;
    for (int i = 2; i < 64; i++) {
        uint8_t* f = (uint8_t*)vt[i];
        if (f < lo || f >= hi) break;
        if (StartsWithDeviceTest(f) && CallsIat(f, 0x400)) return i;
    }
    return -1;
}

// Called at the start of every frame (NotifyRenderBegin, before the original): nothing of the frame is in flight.
static bool ExpectedSize(unsigned* w, unsigned* h) {
    RECT r;
    if (!g_wnd || !o_GetClientRect || !o_GetClientRect(g_wnd, &r) || r.right <= 0 || r.bottom <= 0) return false;
    if (g_scale > 1.001f && g_iatSlot && *g_iatSlot == (void*)Hk_GetClientRect) Hk_GetClientRect(g_wnd, &r);
    *w = r.right; *h = r.bottom;
    return true;
}

void SceneRes_NoteReset() { if (g_askedAt) g_resetSinceAsk = true; }

void SceneRes_OnFrameBegin(void* rr) {
    if (g_gaveUp) {   // until the window size changes
        RECT r;
        if (g_wnd && o_GetClientRect && o_GetClientRect(g_wnd, &r) && (r.right != g_gaveUpW || r.bottom != g_gaveUpH)) { g_gaveUp = false; g_askTries = 0; }
        else { g_pendingResize = false; return; }
    }
    if (g_askedAt && GetTickCount64() - g_askedAt > 3000) {
        // did the game resize? (it ignores the request during loading screens / at start-up): ask again
        g_askedAt = 0;
        unsigned ew, eh;
        if (ExpectedSize(&ew, &eh) && (g_api->pp.BackBufferWidth != ew || g_api->pp.BackBufferHeight != eh)) {
            if (g_resetSinceAsk && g_askTries >= 1) {
                // it reset the device but not to our size (fullscreen windowed sizes from the monitor): asking again
                // would only reset (flash) again and again
                RECT r; o_GetClientRect(g_wnd, &r);
                g_gaveUp = true; g_gaveUpW = r.right; g_gaveUpH = r.bottom;
                LOGW("sceneres: the game resets at %ux%u and ignores SceneScale (fullscreen windowed?) - render scale off; use windowed mode for SceneScale",
                     g_api->pp.BackBufferWidth, g_api->pp.BackBufferHeight);
            } else if (++g_askTries <= 10) {
                LOGI("sceneres: back buffer still %ux%u (want %ux%u) - asking again", g_api->pp.BackBufferWidth, g_api->pp.BackBufferHeight, ew, eh);
                g_pendingResize = true;
            } else if (g_askTries == 11) LOGW("sceneres: the game does not resize - try a window resize or reload the mod");
        } else g_askTries = 0;
        g_resetSinceAsk = false;
    }
    if (!g_pendingResize || !rr) return;
    if (!g_direct && g_gameIatSlot) {   // let the game do it (normal resize path)
        g_pendingResize = false;
        g_lieUntil = GetTickCount64() + 150;
        g_askedAt = GetTickCount64();
        LOGI("sceneres: asking the game to resize the renderer");
        return;
    }
    g_pendingResize = false;
    __try {
        void* impl = *(void**)rr;
        if (!impl) return;
        if (g_resetSlot < 0 && !(GetProcAddress(GetModuleHandleW(L"RemoteRenderer.dll"), "?HandleDeviceReset@RemoteRenderer@bwa@@QEAAXXZ") && g_cfgSlot < 0)) {
            g_resetSlot = g_cfgSlot >= 0 ? g_cfgSlot : FindResetSlot(impl);
            if (g_resetSlot < 0) { LOGW("sceneres: renderer resize method not found - SceneScale has no effect"); return; }
            LOGI("sceneres: renderer resize method = vtable slot %d (+0x%x)", g_resetSlot, g_resetSlot * 8);
        }
        typedef void(__fastcall * PFN)(void*);
        static PFN handleReset = (PFN)GetProcAddress(GetModuleHandleW(L"RemoteRenderer.dll"), "?HandleDeviceReset@RemoteRenderer@bwa@@QEAAXXZ");
        unsigned bw0 = g_api->pp.BackBufferWidth, bh0 = g_api->pp.BackBufferHeight;
        if (handleReset && g_cfgSlot < 0) {   // SceneResDirect=1
            handleReset(rr);
            LOGI("sceneres: renderer resized (HandleDeviceReset): back buffer %ux%u -> %ux%u", bw0, bh0, g_api->pp.BackBufferWidth, g_api->pp.BackBufferHeight);
            return;
        }
        unsigned bw = g_api->pp.BackBufferWidth, bh = g_api->pp.BackBufferHeight;
        ((PFN)(*(void***)impl)[g_resetSlot])(impl);
        LOGI("sceneres: renderer resized: back buffer %ux%u -> %ux%u", bw, bh, g_api->pp.BackBufferWidth, g_api->pp.BackBufferHeight);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOGE("sceneres: renderer resize raised an exception");
    }
}

void SceneRes_Install() {
    g_wnd = g_api->pp.hDeviceWindow ? g_api->pp.hDeviceWindow : g_api->focusWindow;
    D3DCAPS9 caps;
    if (g_api->device && SUCCEEDED(g_api->device->GetDeviceCaps(&caps)) && caps.MaxTextureWidth >= 2048)
        g_maxDim = (LONG)(caps.MaxTextureWidth < caps.MaxTextureHeight ? caps.MaxTextureWidth : caps.MaxTextureHeight);
    if (g_maxDim > 16384) g_maxDim = 16384;
    HMODULE rr = GetModuleHandleW(L"RemoteRenderer.dll");
    if (!rr || !g_wnd) return;
    g_iatSlot = FindIat(rr, "USER32.dll", "GetClientRect");
    if (!g_iatSlot) { LOGW("sceneres: GetClientRect import not found in RemoteRenderer.dll"); return; }
    o_GetClientRect = (PFN_GetClientRect)*g_iatSlot;
    g_gameIatSlot = FindIat(GetModuleHandleW(nullptr), "USER32.dll", "GetClientRect");
    if (g_gameIatSlot && *g_gameIatSlot == (void*)o_GetClientRect) WriteSlot(g_gameIatSlot, (void*)Hk_GameGetClientRect);
    else { LOGW("sceneres: swtor.exe GetClientRect import not found - using the direct renderer reset"); g_gameIatSlot = nullptr; }
    if (g_scale > 1.001f) {
        WrapResetCallback();
        WriteSlot(g_iatSlot, (void*)Hk_GetClientRect);
        LOGI("sceneres: SceneScale %.2f - renderer sees a scaled window; resizing", g_scale);
        g_pendingResize = true;
    } else {
        LOGI("sceneres: SceneScale 1.0 (off)");
        // If a previous core left the renderer at a scaled size, make it re-read the real size.
        unsigned w, h;
        if (SceneRes_RealClientSize(&w, &h) && (g_api->pp.BackBufferWidth != w || g_api->pp.BackBufferHeight != h)) {
            LOGI("sceneres: back buffer %ux%u != window %ux%u - resizing back", g_api->pp.BackBufferWidth, g_api->pp.BackBufferHeight, w, h);
            g_pendingResize = true;
        }
    }
}

void SceneRes_Uninstall() {
    if (g_gameIatSlot && *g_gameIatSlot == (void*)Hk_GameGetClientRect) WriteSlot(g_gameIatSlot, (void*)o_GetClientRect);
    g_gameIatSlot = nullptr;
    g_lieUntil = 0;
    if (g_iatSlot && *g_iatSlot == (void*)Hk_GetClientRect) {
        WriteSlot(g_iatSlot, (void*)o_GetClientRect);
        UnwrapResetCallback();
        // The renderer keeps the scaled size until its next resize; the next core resizes it (back) on its first frame.
    }
    g_iatSlot = nullptr;
    g_pendingResize = false;
}

} // namespace svr
