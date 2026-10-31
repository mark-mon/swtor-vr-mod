// Resource-creation statistics: which pools / usages the game uses (decides whether D3D9Ex is viable, because
// D3D9Ex forbids D3DPOOL_MANAGED), plus which resources it locks. Hooks the d3d9 implementation functions
// through the device vtable with MinHook (removed with everything else on core shutdown).
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstdio>
#include "MinHook.h"
#include "core.h"
#include "common/log.h"

namespace svr {

enum Kind { K_TEX, K_VOL, K_CUBE, K_VB, K_IB, K_RT, K_DS, K_OFF, K_COUNT };
static const char* kKind[K_COUNT] = {"Texture", "VolumeTex", "CubeTex", "VB", "IB", "RenderTarget", "DepthStencil", "Offscreen"};
static std::atomic<uint32_t> g_count[K_COUNT][4];        // [kind][pool]
static std::atomic<uint32_t> g_dynamic[K_COUNT], g_rtUsage[K_COUNT];
static std::atomic<uint32_t> g_managedLogged{0};
static std::atomic<uint32_t> g_lockTexManaged{0}, g_lockTexDefault{0}, g_lockTexOther{0};

typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateTexture)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateVolumeTexture)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateCubeTexture)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateVB)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateIB)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateRT)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateOff)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DPOOL, IDirect3DSurface9**, HANDLE*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_LockRect)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);

static PFN_CreateTexture o_CreateTexture;
static PFN_CreateVolumeTexture o_CreateVolumeTexture;
static PFN_CreateCubeTexture o_CreateCubeTexture;
static PFN_CreateVB o_CreateVB;
static PFN_CreateIB o_CreateIB;
static PFN_CreateRT o_CreateRT, o_CreateDS;
static PFN_CreateOff o_CreateOff;
static PFN_LockRect o_LockRect;

static void Count(Kind k, D3DPOOL pool, DWORD usage) {
    g_count[k][(unsigned)pool < 4 ? pool : 3]++;
    if (usage & D3DUSAGE_DYNAMIC) g_dynamic[k]++;
    if (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) g_rtUsage[k]++;
}
static void LogManaged(Kind k, UINT w, UINT h, UINT levels, DWORD usage, int fmt) {
    if (g_managedLogged++ >= 12) return;
    void* ret = _ReturnAddress();
    HMODULE m = nullptr;
    wchar_t name[MAX_PATH] = L"?";
    // return address of our detour's caller: walk one frame up is not reliable; log the module of _ReturnAddress
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)ret, &m))
        GetModuleFileNameW(m, name, MAX_PATH);
    LOGI("MANAGED %s %ux%u levels %u usage 0x%lx fmt %d", kKind[k], w, h, levels, usage, fmt);
}

static HRESULT STDMETHODCALLTYPE Hk_CreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** t, HANDLE* s) {
    HOOK_GUARD;
    Count(K_TEX, p, u);
    if (p == D3DPOOL_MANAGED) LogManaged(K_TEX, w, h, l, u, f);
    return o_CreateTexture(d, w, h, l, u, f, p, t, s);
}
static HRESULT STDMETHODCALLTYPE Hk_CreateVolumeTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT dd, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DVolumeTexture9** t, HANDLE* s) {
    HOOK_GUARD;
    Count(K_VOL, p, u);
    if (p == D3DPOOL_MANAGED) LogManaged(K_VOL, w, h, l, u, f);
    return o_CreateVolumeTexture(d, w, h, dd, l, u, f, p, t, s);
}
static HRESULT STDMETHODCALLTYPE Hk_CreateCubeTexture(IDirect3DDevice9* d, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DCubeTexture9** t, HANDLE* s) {
    HOOK_GUARD;
    Count(K_CUBE, p, u);
    if (p == D3DPOOL_MANAGED) LogManaged(K_CUBE, e, e, l, u, f);
    return o_CreateCubeTexture(d, e, l, u, f, p, t, s);
}
static HRESULT STDMETHODCALLTYPE Hk_CreateVB(IDirect3DDevice9* d, UINT len, DWORD u, DWORD fvf, D3DPOOL p, IDirect3DVertexBuffer9** b, HANDLE* s) {
    HOOK_GUARD;
    Count(K_VB, p, u);
    if (p == D3DPOOL_MANAGED) LogManaged(K_VB, len, 1, 1, u, (int)fvf);
    return o_CreateVB(d, len, u, fvf, p, b, s);
}
static HRESULT STDMETHODCALLTYPE Hk_CreateIB(IDirect3DDevice9* d, UINT len, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DIndexBuffer9** b, HANDLE* s) {
    HOOK_GUARD;
    Count(K_IB, p, u);
    if (p == D3DPOOL_MANAGED) LogManaged(K_IB, len, 1, 1, u, f);
    return o_CreateIB(d, len, u, f, p, b, s);
}
static HRESULT STDMETHODCALLTYPE Hk_CreateRT(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL l, IDirect3DSurface9** o, HANDLE* s) {
    HOOK_GUARD;
    Count(K_RT, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET);
    LOGI("CreateRenderTarget %ux%u fmt %d ms %d lockable %d", w, h, f, ms, l);
    return o_CreateRT(d, w, h, f, ms, q, l, o, s);
}
static HRESULT STDMETHODCALLTYPE Hk_CreateDS(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE ms, DWORD q, BOOL l, IDirect3DSurface9** o, HANDLE* s) {
    HOOK_GUARD;
    Count(K_DS, D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL);
    LOGI("CreateDepthStencil %ux%u fmt %d ms %d", w, h, f, ms);
    return o_CreateDS(d, w, h, f, ms, q, l, o, s);
}
static HRESULT STDMETHODCALLTYPE Hk_CreateOff(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DPOOL p, IDirect3DSurface9** o, HANDLE* s) {
    HOOK_GUARD;
    Count(K_OFF, p, 0);
    return o_CreateOff(d, w, h, f, p, o, s);
}
static HRESULT STDMETHODCALLTYPE Hk_LockRect(IDirect3DTexture9* t, UINT lvl, D3DLOCKED_RECT* r, const RECT* rc, DWORD fl) {
    HOOK_GUARD;
    D3DSURFACE_DESC d;
    if (SUCCEEDED(t->GetLevelDesc(lvl, &d))) {
        if (d.Pool == D3DPOOL_MANAGED) g_lockTexManaged++;
        else if (d.Pool == D3DPOOL_DEFAULT) g_lockTexDefault++;
        else g_lockTexOther++;
    }
    return o_LockRect(t, lvl, r, rc, fl);
}

static bool Hook(void* target, void* detour, void** orig, const char* name) {
    if (MH_CreateHook(target, detour, orig) != MH_OK || MH_EnableHook(target) != MH_OK) { LOGW("res_stats: hook %s failed", name); return false; }
    return true;
}

static bool g_installed = false;

void ResStats_Install(IDirect3DDevice9* dev) {
    if (g_installed || !dev) return;
    void** vt = *(void***)dev;
    Hook(vt[23], (void*)Hk_CreateTexture, (void**)&o_CreateTexture, "CreateTexture");
    Hook(vt[24], (void*)Hk_CreateVolumeTexture, (void**)&o_CreateVolumeTexture, "CreateVolumeTexture");
    Hook(vt[25], (void*)Hk_CreateCubeTexture, (void**)&o_CreateCubeTexture, "CreateCubeTexture");
    Hook(vt[26], (void*)Hk_CreateVB, (void**)&o_CreateVB, "CreateVertexBuffer");
    Hook(vt[27], (void*)Hk_CreateIB, (void**)&o_CreateIB, "CreateIndexBuffer");
    Hook(vt[28], (void*)Hk_CreateRT, (void**)&o_CreateRT, "CreateRenderTarget");
    Hook(vt[29], (void*)Hk_CreateDS, (void**)&o_CreateDS, "CreateDepthStencilSurface");
    Hook(vt[36], (void*)Hk_CreateOff, (void**)&o_CreateOff, "CreateOffscreenPlainSurface");
    // Texture LockRect: get the texture vtable from a temporary texture.
    IDirect3DTexture9* t = nullptr;
    if (SUCCEEDED(o_CreateTexture ? o_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &t, nullptr)
                                  : dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &t, nullptr)) && t) {
        Hook((*(void***)t)[19], (void*)Hk_LockRect, (void**)&o_LockRect, "Texture::LockRect");
        t->Release();
    }
    g_installed = true;
    LOGI("res_stats: resource creation hooks installed");
}

void ResStats_Log() {
    if (!g_installed) return;
    char line[512];
    int n = 0;
    for (int k = 0; k < K_COUNT; k++) {
        uint32_t d = g_count[k][0], m = g_count[k][1], s = g_count[k][2], sc = g_count[k][3];
        if (d + m + s + sc == 0) continue;
        n += snprintf(line + n, sizeof(line) - n, " %s[def %u man %u sys %u scr %u dyn %u rt %u]", kKind[k], d, m, s, sc,
                      g_dynamic[k].load(), g_rtUsage[k].load());
    }
    LOGI("resources since load:%s", n ? line : " none");
    LOGI("texture LockRect since load: managed %u default %u other %u", g_lockTexManaged.load(), g_lockTexDefault.load(), g_lockTexOther.load());
}

void ResStats_Reset() { g_installed = false; }

} // namespace svr

// ---- video memory budget (DXGI 1.4): the D3D9 device's process usage vs the budget Windows gives it
#include <dxgi1_4.h>
namespace svr {
static IDXGIAdapter3* g_vmAdapter = nullptr;
static bool g_vmTried = false;
void VidMem_Log() {
    if (!g_vmTried) {
        g_vmTried = true;
        HMODULE dx = LoadLibraryW(L"dxgi.dll");
        typedef HRESULT(WINAPI * PFN)(REFIID, void**);
        PFN create = dx ? (PFN)GetProcAddress(dx, "CreateDXGIFactory1") : nullptr;
        IDXGIFactory1* f = nullptr;
        if (create && SUCCEEDED(create(__uuidof(IDXGIFactory1), (void**)&f))) {
            // the adapter the game's device is on (D3D9 adapter 0 = the primary DXGI adapter in practice)
            IDXGIAdapter1* a = nullptr;
            if (SUCCEEDED(f->EnumAdapters1(0, &a))) { a->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&g_vmAdapter); a->Release(); }
            f->Release();
        }
        if (!g_vmAdapter) LOGW("vidmem: DXGI 1.4 adapter not available");
    }
    if (!g_vmAdapter) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO l = {}, n = {};
    g_vmAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &l);
    g_vmAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &n);
    LOGI("vidmem: local %llu / budget %llu MB%s, system (non-local) %llu MB, back buffer %ux%u",
         (unsigned long long)(l.CurrentUsage >> 20), (unsigned long long)(l.Budget >> 20), l.CurrentUsage > l.Budget ? " OVER BUDGET" : "",
         (unsigned long long)(n.CurrentUsage >> 20), g_api->pp.BackBufferWidth, g_api->pp.BackBufferHeight);
}
void VidMem_Release() { if (g_vmAdapter) { g_vmAdapter->Release(); g_vmAdapter = nullptr; } g_vmTried = false; }
}
