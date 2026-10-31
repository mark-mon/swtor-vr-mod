// Frame tracer: records the ordered sequence of renderer-API calls and D3D9 device calls for a few frames and
// writes them to the core log. Used to learn where the 3D scene ends and the UI starts inside one frame, and
// whether DrawScene can be replayed.
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include "MinHook.h"
#include "core.h"
#include "common/log.h"
#include "common/config.h"

namespace svr {

static std::atomic<int> g_traceFrames{0};   // frames still to trace (counted at Present)
static bool g_tracing = false;
static char g_buf[256 * 1024];
static size_t g_len = 0;
static int g_depth = 0;
static uint32_t g_nDraws = 0, g_lastDraws = 0;
uint32_t Trace_LastFrameDraws() { return g_lastDraws; }

bool Trace_Active() { return g_tracing; }

void Trace_Event(const char* fmt, ...) {
    if (!g_tracing || g_len > sizeof(g_buf) - 512) return;
    int n = snprintf(g_buf + g_len, sizeof(g_buf) - g_len, "%*s", g_depth * 2, "");
    if (n > 0) g_len += n;
    va_list ap;
    va_start(ap, fmt);
    n = vsnprintf(g_buf + g_len, sizeof(g_buf) - g_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_len += n;
    if (g_len < sizeof(g_buf) - 1) g_buf[g_len++] = '\n';
}
void Trace_Enter(const char* name) { Trace_Event("> %s", name); g_depth++; }
void Trace_Leave(const char* name) { if (g_depth > 0) g_depth--; Trace_Event("< %s", name); }

void Trace_Request(int frames) { g_traceFrames = frames; }

// Called at the very start of the loader Present callback (frame boundary).
void Trace_FrameBoundary() {
    if (g_tracing) {
        Trace_Event("== Present (draws this frame: %u)", g_nDraws);
        // flush the buffer line by line into the log
        g_buf[g_len] = 0;
        char* p = g_buf;
        while (*p) {
            char* nl = strchr(p, '\n');
            if (nl) *nl = 0;
            LOGI("T %s", p);
            if (!nl) break;
            p = nl + 1;
        }
        g_len = 0;
        g_depth = 0;
        g_tracing = false;
    }
    g_lastDraws = g_nDraws;
    g_nDraws = 0;
    if (g_traceFrames > 0) {
        g_traceFrames--;
        g_tracing = true;
        LOGI("T ===== trace frame begins =====");
    }
}

// ---- device hooks (only log while tracing; cheap otherwise)
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetRT)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SetDS)(IDirect3DDevice9*, IDirect3DSurface9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Void)(IDirect3DDevice9*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Clear)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DIP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_DP)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_StretchRect)(IDirect3DDevice9*, IDirect3DSurface9*, const RECT*, IDirect3DSurface9*, const RECT*, D3DTEXTUREFILTERTYPE);
typedef HRESULT(STDMETHODCALLTYPE* PFN_VSConst)(IDirect3DDevice9*, UINT, const float*, UINT);
static PFN_SetRT o_SetRT;
static PFN_SetDS o_SetDS;
static PFN_Void o_BeginScene, o_EndScene;
static PFN_Clear o_Clear;
static PFN_DIP o_DIP;
static PFN_DP o_DP;
static PFN_StretchRect o_StretchRect;
static PFN_VSConst o_VSConst;

// ---- Shadow reuse: the right-eye pass (stereo pass 2) skips drawing into the shadow atlas and keeps the one the left
// eye just rendered (the light and the scene are the same; the eyes are a few cm apart). Shadow atlas = R32F render
// target with width = 2 x height (8192x4096 at the highest setting). Config ShadowReuse = 1.
static bool g_shadowReuse = false;
static bool g_skipping = false;
static IDirect3DSurface9* g_lastRt0 = nullptr;
static bool g_lastRt0Shadow = false;
static uint64_t g_skippedDraws = 0, g_skippedPasses = 0;

void Trace_LoadSettings(const xrb::Config& c) { g_shadowReuse = c.GetInt("ShadowReuse", 0) != 0; }   // experimental, off: shadows are interleaved with the main pass

static bool IsShadowTarget(IDirect3DSurface9* s) {
    if (!s) return false;
    if (s == g_lastRt0) return g_lastRt0Shadow;
    D3DSURFACE_DESC d;
    bool sh = SUCCEEDED(s->GetDesc(&d)) && d.Format == D3DFMT_R32F && d.Width >= 1024 && d.Width == 2 * d.Height;
    g_lastRt0 = s; g_lastRt0Shadow = sh;
    return sh;
}
static inline bool Skip() {
    if (!g_skipping) return false;
    if (Stereo_Pass() != 2) { g_skipping = false; return false; }
    g_skippedDraws++;
    return true;
}
void Trace_LogShadowStats() {
    if (g_shadowReuse) LOGI("shadow reuse: right eye skipped %llu shadow passes / %llu draws", (unsigned long long)g_skippedPasses, (unsigned long long)g_skippedDraws);
    g_skippedDraws = g_skippedPasses = 0;
}

static void Desc(IDirect3DSurface9* s, char* out, size_t n) {
    if (!s) { snprintf(out, n, "null"); return; }
    D3DSURFACE_DESC d;
    if (FAILED(s->GetDesc(&d))) { snprintf(out, n, "%p", s); return; }
    snprintf(out, n, "%p %ux%u fmt %d", s, d.Width, d.Height, d.Format);
}

static HRESULT STDMETHODCALLTYPE Hk_SetRT(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
    HOOK_GUARD;
    s = Hud_FilterRT(i, s);
    if (g_tracing) { char b[96]; Desc(s, b, sizeof(b)); Trace_Event("SetRenderTarget[%lu] %s  (draws so far %u)", i, b, g_nDraws); }
    if (i == 0) {
        bool skip = g_shadowReuse && Stereo_Pass() == 2 && IsShadowTarget(s);
        if (skip && !g_skipping) g_skippedPasses++;
        g_skipping = skip;
    }
    return o_SetRT(d, i, s);
}
static HRESULT STDMETHODCALLTYPE Hk_SetDS(IDirect3DDevice9* d, IDirect3DSurface9* s) {
    HOOK_GUARD;
    if (g_tracing) { char b[96]; Desc(s, b, sizeof(b)); Trace_Event("SetDepthStencil %s", b); }
    return o_SetDS(d, s);
}
static HRESULT STDMETHODCALLTYPE Hk_BeginScene(IDirect3DDevice9* d) { HOOK_GUARD; if (g_tracing) Trace_Event("BeginScene"); return o_BeginScene(d); }
static HRESULT STDMETHODCALLTYPE Hk_EndScene(IDirect3DDevice9* d) { HOOK_GUARD; if (g_tracing) Trace_Event("EndScene (draws %u)", g_nDraws); return o_EndScene(d); }
static HRESULT STDMETHODCALLTYPE Hk_Clear(IDirect3DDevice9* d, DWORD c, const D3DRECT* r, DWORD f, D3DCOLOR col, float z, DWORD s) {
    HOOK_GUARD;
    if (g_tracing) Trace_Event("Clear flags 0x%lx color %08lx z %.3f", f, col, z);
    if (Skip()) return D3D_OK;
    return o_Clear(d, c, r, f, col, z, s);
}
static HRESULT STDMETHODCALLTYPE Hk_DIP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT b, UINT mi, UINT nv, UINT si, UINT pc) {
    HOOK_GUARD;
    g_nDraws++;
    Hud_OnDraw();
    if (Skip()) return D3D_OK;
    return o_DIP(d, t, b, mi, nv, si, pc);
}
static HRESULT STDMETHODCALLTYPE Hk_DP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT sv, UINT pc) {
    HOOK_GUARD;
    g_nDraws++;
    Hud_OnDraw();
    if (Skip()) return D3D_OK;
    return o_DP(d, t, sv, pc);
}
static HRESULT STDMETHODCALLTYPE Hk_StretchRect(IDirect3DDevice9* d, IDirect3DSurface9* s, const RECT* sr, IDirect3DSurface9* t, const RECT* tr, D3DTEXTUREFILTERTYPE f) {
    HOOK_GUARD;
    if (g_tracing) { char a[96], b[96]; Desc(s, a, sizeof(a)); Desc(t, b, sizeof(b)); Trace_Event("StretchRect %s -> %s", a, b); }
    return o_StretchRect(d, s, sr, t, tr, f);
}

static bool H(void* t, void* d, void** o) { return MH_CreateHook(t, d, o) == MH_OK && MH_EnableHook(t) == MH_OK; }

void Trace_InstallDeviceHooks(IDirect3DDevice9* dev) {
    void** vt = *(void***)dev;
    int ok = 0;
    ok += H(vt[37], (void*)Hk_SetRT, (void**)&o_SetRT);
    ok += H(vt[39], (void*)Hk_SetDS, (void**)&o_SetDS);
    ok += H(vt[41], (void*)Hk_BeginScene, (void**)&o_BeginScene);
    ok += H(vt[42], (void*)Hk_EndScene, (void**)&o_EndScene);
    ok += H(vt[43], (void*)Hk_Clear, (void**)&o_Clear);
    ok += H(vt[34], (void*)Hk_StretchRect, (void**)&o_StretchRect);
    ok += H(vt[81], (void*)Hk_DP, (void**)&o_DP);
    ok += H(vt[82], (void*)Hk_DIP, (void**)&o_DIP);
    LOGI("trace: %d/8 device hooks", ok);
}

} // namespace svr
