// SWTOR VR loader: d3d9.dll proxy that stays loaded for the whole game session and hot-reloads the core DLL.
// See src/common/loader_api.h for the contract with the core.
//
// Install: copy d3d9.dll (this) next to swtor.exe (swtor\retailclient\), and the core to
//          swtor\retailclient\swtorvr\swtorvr_core.dll.
// Reload triggers (the swap itself always happens on the render thread inside Present):
//   - swtorvr_core.dll changes on disk (AutoReload = 1; waits until the file is stable for 1 s)
//   - named event Local\SwtorVR_Reload is set (e.g. from PowerShell: [Threading.EventWaitHandle]::OpenExisting(...).Set())
//   - Ctrl+Shift+F12 in the game window
// Unload only (core removed, game keeps running untouched): Local\SwtorVR_Unload or Ctrl+Shift+F11.
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>
#include <string>
#include <atomic>
#include "../common/loader_api.h"
#include "../common/log.h"
#include "../common/config.h"

using namespace svr;

#define LOADER_VERSION "1.0.0"

// ---------------------------------------------------------------------------------------------------------------
// globals
static HMODULE g_self = nullptr;
static HMODULE g_real = nullptr;
static std::wstring g_gameDir, g_modDir;
static LoaderApi g_api = {};
static CoreCallbacks g_cb = {};
static CRITICAL_SECTION g_coreCs;          // protects core load/unload vs. device callbacks
static HMODULE g_core = nullptr;
static PFN_CoreShutdown g_coreShutdown = nullptr;
struct Retired { HMODULE mod; PFN_CoreCanUnload canUnload; PFN_CoreFinalize finalize; DWORD since; bool warned; };
static Retired g_retired[8];
static int g_nRetired = 0;
static std::atomic<int> g_reloadReq{0};    // 1 = reload, 2 = unload
static std::atomic<bool> g_started{false};
static int g_mode = D3D9_NATIVE;
static float g_sceneScale = 1.0f;                 // back buffer = game's requested size * this (3D scene resolution)
static UINT g_gameW = 0, g_gameH = 0;              // what the game asked for (its UI is laid out for this)

// Enlarge the back buffer the game asked for. The renderer draws the 3D scene at the back buffer size; the UI keeps
// the game's own size (the core captures it into a HUD texture of that size). Present scales the result to the window.
static void ScalePP(D3DPRESENT_PARAMETERS* pp) {
    g_gameW = pp->BackBufferWidth;
    g_gameH = pp->BackBufferHeight;
    if (g_sceneScale > 1.001f && pp->Windowed && g_gameW && g_gameH) {
        pp->BackBufferWidth = (UINT)(g_gameW * g_sceneScale + 0.5f) & ~1u;
        pp->BackBufferHeight = (UINT)(g_gameH * g_sceneScale + 0.5f) & ~1u;
        LOGI("SceneScale %.2f: back buffer %ux%u -> %ux%u", g_sceneScale, g_gameW, g_gameH, pp->BackBufferWidth, pp->BackBufferHeight);
    }
}
static void UnscalePP(D3DPRESENT_PARAMETERS* pp) {
    // The game reads its parameters back after the call; give it the size it asked for.
    if (g_gameW && g_gameH) { pp->BackBufferWidth = g_gameW; pp->BackBufferHeight = g_gameH; }
}

// For the core (optional export, looked up by name so the loader API version does not change).
extern "C" __declspec(dllexport) void __cdecl SwtorVR_LoaderGameSize(unsigned* w, unsigned* h) {
    if (w) *w = g_gameW;
    if (h) *h = g_gameH;
}
static bool g_loadCore = true, g_autoReload = true;
static FILETIME g_coreTime = {};
static uint32_t g_runCounter = 0;

static void ApiLog(int level, const char* msg) { xrb::LogWrite(level, "%s", msg); }

// ---------------------------------------------------------------------------------------------------------------
// real d3d9
static HMODULE Real() {
    if (!g_real) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        std::wstring p = std::wstring(sys) + L"\\d3d9.dll";
        g_real = LoadLibraryW(p.c_str());
    }
    return g_real;
}
static FARPROC RealProc(const char* name) { HMODULE m = Real(); return m ? GetProcAddress(m, name) : nullptr; }

// Generic forwarders for the rarely used exports (all take integer/pointer arguments only).
typedef uintptr_t(WINAPI* Gen8)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);
#define FWD(name)                                                                                                 \
    extern "C" uintptr_t WINAPI Proxy_##name(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e,     \
                                             uintptr_t f, uintptr_t g, uintptr_t h) {                             \
        static Gen8 p = (Gen8)RealProc(#name);                                                                    \
        return p ? p(a, b, c, d, e, f, g, h) : 0;                                                                 \
    }
FWD(D3DPERF_BeginEvent) FWD(D3DPERF_EndEvent) FWD(D3DPERF_GetStatus) FWD(D3DPERF_QueryRepeatFrame)
FWD(D3DPERF_SetMarker) FWD(D3DPERF_SetOptions) FWD(D3DPERF_SetRegion) FWD(DebugSetLevel) FWD(DebugSetMute)
FWD(Direct3D9EnableMaximizedWindowedModeShim) FWD(Direct3DCreate9On12) FWD(Direct3DCreate9On12Ex)
FWD(Direct3DShaderValidatorCreate9) FWD(PSGPError) FWD(PSGPSampleTexture)

// ---------------------------------------------------------------------------------------------------------------
// vtable patching
static void* PatchVtable(void* obj, int index, void* hook) {
    void** vt = *(void***)obj;
    void* orig = vt[index];
    if (orig == hook) return nullptr;      // already patched
    DWORD old;
    if (!VirtualProtect(&vt[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return nullptr;
    vt[index] = hook;
    VirtualProtect(&vt[index], sizeof(void*), old, &old);
    FlushInstructionCache(GetCurrentProcess(), &vt[index], sizeof(void*));
    return orig;
}

// ---------------------------------------------------------------------------------------------------------------
// core load / unload (call with g_coreCs held, on the render thread once a device exists)
static bool GetFileTime(const std::wstring& p, FILETIME* ft, ULONGLONG* size) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) return false;
    *ft = fa.ftLastWriteTime;
    if (size) *size = ((ULONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    return true;
}

// Phase 1: the core removes its hooks; the module stays loaded until no thread is inside it (FreeRetired).
static void UnloadCore(const char* why) {
    if (!g_core) return;
    LOGI("retiring core (%s)", why);
    g_cb = {};                                   // no more callbacks into it
    if (g_coreShutdown) g_coreShutdown();
    Retired r = { g_core, (PFN_CoreCanUnload)GetProcAddress(g_core, "SwtorVR_CoreCanUnload"),
                  (PFN_CoreFinalize)GetProcAddress(g_core, "SwtorVR_CoreFinalize"), GetTickCount(), false };
    if (g_nRetired < 8) g_retired[g_nRetired++] = r;
    else LOGE("too many retired cores; leaking one (never freed)");
    g_core = nullptr;
    g_coreShutdown = nullptr;
}

// Phase 2: free retired cores once they report that no thread is inside them. Called at the start of each Present.
static void FreeRetired() {
    for (int i = 0; i < g_nRetired;) {
        Retired& r = g_retired[i];
        if (r.canUnload && !r.canUnload()) {
            if (!r.warned && GetTickCount() - r.since > 10000) { LOGW("retired core still busy after 10 s; keeping it loaded"); r.warned = true; }
            i++;
            continue;
        }
        if (r.finalize) r.finalize();
        FreeLibrary(r.mod);
        LOGI("retired core freed after %lu ms", GetTickCount() - r.since);
        g_retired[i] = g_retired[--g_nRetired];
    }
}

static void CleanRunDir() {
    std::wstring dir = g_modDir + L"run\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + fd.cFileName).c_str());   // fails for loaded ones
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static void LoadCore() {
    if (!g_loadCore) return;
    std::wstring src = g_modDir + L"swtorvr_core.dll";
    if (!GetFileTime(src, &g_coreTime, nullptr)) { LOGW("no core at swtorvr\\swtorvr_core.dll - running without"); return; }
    CreateDirectoryW((g_modDir + L"run").c_str(), nullptr);
    CleanRunDir();
    wchar_t name[64];
    swprintf(name, 64, L"run\\core_%u_%u", GetCurrentProcessId(), g_runCounter++);
    std::wstring dst = g_modDir + name + L".dll";
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) { LOGE("copy core failed (%lu)", GetLastError()); return; }
    std::wstring pdbSrc = g_modDir + L"swtorvr_core.pdb", pdbDst = g_modDir + name + L".pdb";
    CopyFileW(pdbSrc.c_str(), pdbDst.c_str(), FALSE);
    HMODULE m = LoadLibraryW(dst.c_str());
    if (!m) { LOGE("LoadLibrary core failed (%lu)", GetLastError()); return; }
    auto init = (PFN_CoreInit)GetProcAddress(m, "SwtorVR_CoreInit");
    auto shut = (PFN_CoreShutdown)GetProcAddress(m, "SwtorVR_CoreShutdown");
    auto ver = (PFN_CoreVersion)GetProcAddress(m, "SwtorVR_CoreVersion");
    if (!init || !shut) { LOGE("core lacks SwtorVR_CoreInit/Shutdown exports"); FreeLibrary(m); return; }
    CoreCallbacks cb = {};
    cb.size = sizeof(cb);
    LOGI("core load #%u: %ls  version %s", g_api.reloadCount, name, ver ? ver() : "?");
    if (!init(&g_api, &cb)) {
        LOGW("core refused to start (CoreInit=false); retiring it");
        g_core = m; g_coreShutdown = shut; UnloadCore("init refused");
        return;
    }
    g_core = m;
    g_coreShutdown = shut;
    g_cb = cb;
    g_api.reloadCount++;
    if (g_api.device && g_cb.OnDeviceCreated) g_cb.OnDeviceCreated(g_api.device, &g_api.pp);
}

static void ServiceReload() {
    if (g_nRetired) { EnterCriticalSection(&g_coreCs); FreeRetired(); LeaveCriticalSection(&g_coreCs); }
    int req = g_reloadReq.exchange(0);
    if (!req) return;
    EnterCriticalSection(&g_coreCs);
    UnloadCore(req == 2 ? "unload request" : "reload request");
    if (req == 1) LoadCore();
    LeaveCriticalSection(&g_coreCs);
}

static DWORD WINAPI WatchThread(void*) {
    HANDLE ev[2];
    ev[0] = CreateEventW(nullptr, FALSE, FALSE, L"Local\\SwtorVR_Reload");
    ev[1] = CreateEventW(nullptr, FALSE, FALSE, L"Local\\SwtorVR_Unload");
    std::wstring src = g_modDir + L"swtorvr_core.dll";
    FILETIME seen = g_coreTime, pending = {};
    DWORD pendingSince = 0;
    for (;;) {
        DWORD r = WaitForMultipleObjects(2, ev, FALSE, 500);
        if (r == WAIT_OBJECT_0) { LOGI("reload requested (event)"); g_reloadReq = 1; }
        else if (r == WAIT_OBJECT_0 + 1) { LOGI("unload requested (event)"); g_reloadReq = 2; }
        if (!g_autoReload) continue;
        FILETIME ft; ULONGLONG sz = 0;
        if (!GetFileTime(src, &ft, &sz) || sz == 0) continue;
        if (CompareFileTime(&ft, &seen) == 0) { pendingSince = 0; continue; }
        if (CompareFileTime(&ft, &pending) != 0) { pending = ft; pendingSince = GetTickCount(); continue; }
        if (GetTickCount() - pendingSince < 1000) continue;
        HANDLE h = CreateFileW(src.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);   // still being written?
        if (h == INVALID_HANDLE_VALUE) continue;
        CloseHandle(h);
        seen = ft;
        LOGI("core file changed on disk -> reload");
        g_reloadReq = 1;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// device hooks
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_Reset)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResetEx)(IDirect3DDevice9Ex*, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*);
typedef HRESULT(STDMETHODCALLTYPE* PFN_PresentEx)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_SCPresent)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
typedef HRESULT(STDMETHODCALLTYPE* PFN_CreateDeviceEx)(IDirect3D9Ex*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, D3DDISPLAYMODEEX*, IDirect3DDevice9Ex**);

static PFN_Present o_Present;
static PFN_Reset o_Reset;
static PFN_ResetEx o_ResetEx;
static PFN_PresentEx o_PresentEx;
static PFN_SCPresent o_SCPresent;
static PFN_CreateDevice o_CreateDevice;
static PFN_CreateDeviceEx o_CreateDeviceEx;
static std::atomic<uint64_t> g_nPresent{0}, g_nSCPresent{0}, g_nPresentEx{0};

static void Hotkeys() {
    static bool down12 = false, down11 = false;
    bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    bool k12 = mods && (GetAsyncKeyState(VK_F12) & 0x8000), k11 = mods && (GetAsyncKeyState(VK_F11) & 0x8000);
    if (k12 && !down12) { LOGI("reload requested (Ctrl+Shift+F12)"); g_reloadReq = 1; }
    if (k11 && !down11) { LOGI("unload requested (Ctrl+Shift+F11)"); g_reloadReq = 2; }
    down12 = k12; down11 = k11;
}

static void FrameStart(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc) {
    g_api.presentCount++;
    if ((g_api.presentCount & 7) == 0) Hotkeys();
    ServiceReload();
    if (g_cb.OnPresentBegin) g_cb.OnPresentBegin(dev, sc);
}
static void FrameEnd(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc, HRESULT hr) {
    if (g_cb.OnPresentEnd) g_cb.OnPresentEnd(dev, sc, hr);
    static DWORD last = GetTickCount();
    DWORD now = GetTickCount();
    if (now - last >= 10000) {
        LOGI("presents: device %llu, swapchain %llu, ex %llu (core %s, loads %u)", (unsigned long long)g_nPresent.load(),
             (unsigned long long)g_nSCPresent.load(), (unsigned long long)g_nPresentEx.load(), g_core ? "loaded" : "none",
             g_api.reloadCount);
        last = now;
    }
}

static HRESULT STDMETHODCALLTYPE Hk_Present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    if (dev != g_api.device) return o_Present(dev, a, b, w, r);
    g_nPresent++;
    FrameStart(dev, nullptr);
    HRESULT hr = o_Present(dev, a, b, w, r);
    FrameEnd(dev, nullptr, hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_PresentEx(IDirect3DDevice9Ex* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD f) {
    if ((IDirect3DDevice9*)dev != g_api.device) return o_PresentEx(dev, a, b, w, r, f);
    g_nPresentEx++;
    FrameStart(dev, nullptr);
    HRESULT hr = o_PresentEx(dev, a, b, w, r, f);
    FrameEnd(dev, nullptr, hr);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_SCPresent(IDirect3DSwapChain9* sc, const RECT* a, const RECT* b, HWND w, const RGNDATA* r, DWORD f) {
    IDirect3DDevice9* dev = nullptr;
    sc->GetDevice(&dev);
    if (dev) dev->Release();
    if (!dev || dev != g_api.device) return o_SCPresent(sc, a, b, w, r, f);
    g_nSCPresent++;
    FrameStart(dev, sc);
    HRESULT hr = o_SCPresent(sc, a, b, w, r, f);
    FrameEnd(dev, sc, hr);
    return hr;
}
static void LogPP(const char* tag, const D3DPRESENT_PARAMETERS* pp) {
    LOGI("%s: %ux%u fmt %d count %u ms %d swap %d windowed %d hwnd %p ds %d/%d flags 0x%x refresh %u interval 0x%x", tag,
         pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat, pp->BackBufferCount, pp->MultiSampleType,
         pp->SwapEffect, pp->Windowed, pp->hDeviceWindow, pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat,
         pp->Flags, pp->FullScreen_RefreshRateInHz, pp->PresentationInterval);
}
static HRESULT STDMETHODCALLTYPE Hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    if (dev != g_api.device) return o_Reset(dev, pp);
    EnterCriticalSection(&g_coreCs);
    ScalePP(pp);
    LogPP("Reset", pp);
    if (g_cb.OnPreReset) g_cb.OnPreReset(dev, pp);
    HRESULT hr = o_Reset(dev, pp);
    if (SUCCEEDED(hr)) g_api.pp = *pp;
    else LOGW("Reset failed 0x%08lx", hr);
    UnscalePP(pp);
    if (g_cb.OnPostReset) g_cb.OnPostReset(dev, pp, hr);
    LeaveCriticalSection(&g_coreCs);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_ResetEx(IDirect3DDevice9Ex* dev, D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* m) {
    if ((IDirect3DDevice9*)dev != g_api.device) return o_ResetEx(dev, pp, m);
    EnterCriticalSection(&g_coreCs);
    ScalePP(pp);
    LogPP("ResetEx", pp);
    if (g_cb.OnPreReset) g_cb.OnPreReset(dev, pp);
    HRESULT hr = o_ResetEx(dev, pp, m);
    if (SUCCEEDED(hr)) g_api.pp = *pp;
    UnscalePP(pp);
    if (g_cb.OnPostReset) g_cb.OnPostReset(dev, pp, hr);
    LeaveCriticalSection(&g_coreCs);
    return hr;
}

static void OnNewDevice(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp, HWND focus) {
    if (g_api.device) LOGW("a second device was created; switching to it (old %p)", g_api.device);
    g_api.device = dev;
    g_api.pp = *pp;
    g_api.focusWindow = focus;
    LogPP("CreateDevice", pp);
    void* o;
    if ((o = PatchVtable(dev, 17, (void*)Hk_Present))) o_Present = (PFN_Present)o;
    if ((o = PatchVtable(dev, 16, (void*)Hk_Reset))) o_Reset = (PFN_Reset)o;
    IDirect3DDevice9Ex* ex = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), (void**)&ex)) && ex) {
        if ((o = PatchVtable(ex, 121, (void*)Hk_PresentEx))) o_PresentEx = (PFN_PresentEx)o;
        if ((o = PatchVtable(ex, 132, (void*)Hk_ResetEx))) o_ResetEx = (PFN_ResetEx)o;
        ex->Release();
        LOGI("device is IDirect3DDevice9Ex");
    }
    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        if ((o = PatchVtable(sc, 3, (void*)Hk_SCPresent))) o_SCPresent = (PFN_SCPresent)o;
        sc->Release();
    }
    D3DCAPS9 caps;
    dev->GetDeviceCaps(&caps);
    D3DADAPTER_IDENTIFIER9 id = {};
    if (g_api.d3d) g_api.d3d->GetAdapterIdentifier(caps.AdapterOrdinal, 0, &id);
    LOGI("device %p on adapter %u (%s), creating thread %lu", dev, caps.AdapterOrdinal, id.Description, GetCurrentThreadId());
    EnterCriticalSection(&g_coreCs);
    if (!g_core) LoadCore();
    else if (g_cb.OnDeviceCreated) g_cb.OnDeviceCreated(dev, &g_api.pp);
    LeaveCriticalSection(&g_coreCs);
}

static HRESULT STDMETHODCALLTYPE Hk_CreateDevice(IDirect3D9* d3d, UINT ad, D3DDEVTYPE t, HWND w, DWORD fl,
                                                 D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    LOGI("CreateDevice adapter %u type %d flags 0x%lx (%s)", ad, t, fl, g_mode == D3D9_EX ? "as Ex" : "native");
    ScalePP(pp);
    HRESULT hr;
    if (g_mode == D3D9_EX && o_CreateDeviceEx) {
        // pp->Windowed must be TRUE (or a display mode is required); SWTOR runs windowed / borderless for VR.
        D3DDISPLAYMODEEX dm = { sizeof(dm), pp->BackBufferWidth, pp->BackBufferHeight, pp->FullScreen_RefreshRateInHz,
                                pp->BackBufferFormat, D3DSCANLINEORDERING_PROGRESSIVE };
        hr = o_CreateDeviceEx((IDirect3D9Ex*)d3d, ad, t, w, fl, pp, pp->Windowed ? nullptr : &dm, (IDirect3DDevice9Ex**)out);
    } else {
        hr = o_CreateDevice(d3d, ad, t, w, fl, pp, out);
    }
    if (FAILED(hr) || !out || !*out) { LOGE("CreateDevice failed 0x%08lx", hr); UnscalePP(pp); return hr; }
    OnNewDevice(*out, pp, w);
    UnscalePP(pp);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hk_CreateDeviceEx(IDirect3D9Ex* d3d, UINT ad, D3DDEVTYPE t, HWND w, DWORD fl,
                                                   D3DPRESENT_PARAMETERS* pp, D3DDISPLAYMODEEX* m, IDirect3DDevice9Ex** out) {
    HRESULT hr = o_CreateDeviceEx(d3d, ad, t, w, fl, pp, m, out);
    if (SUCCEEDED(hr) && out && *out) OnNewDevice(*out, pp, w);
    return hr;
}

// ---------------------------------------------------------------------------------------------------------------
// startup (first Direct3DCreate9 call: outside the loader lock)
static LONG CALLBACK CrashVeh(EXCEPTION_POINTERS* ep) {
    static std::atomic<int> n{0};
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_STACK_OVERFLOW &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != 0xC0000409 /*stack buffer overrun*/)
        return EXCEPTION_CONTINUE_SEARCH;
    if (n++ >= 20) return EXCEPTION_CONTINUE_SEARCH;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    HMODULE m = nullptr;
    wchar_t mod[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)addr, &m))
        GetModuleFileNameW(m, mod, MAX_PATH);
    const wchar_t* base = wcsrchr(mod, L'\\');
    LOGE("exception 0x%08lx at %ls+0x%llx (thread %lu) info %llx %llx  [first-chance; the game may handle it]", code,
         base ? base + 1 : mod, (unsigned long long)((uintptr_t)addr - (uintptr_t)m), GetCurrentThreadId(),
         (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
         (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    xrb::LogFlush();
    return EXCEPTION_CONTINUE_SEARCH;
}

static const char* kDefaultConfig =
    "# SWTOR VR loader settings (d3d9.dll). Read once when the game starts.\n"
    "\n"
    "# How the game's D3D9 device is created: ex | native | on12\n"
    "#   ex = D3D9Ex (default; REQUIRED for VR output - only D3D9Ex can share frames with the VR host).\n"
    "#   native = untouched (flat game, mod logs only).  on12 = D3D9-on-12 (experimental).\n"
    "D3D9Mode = ex\n"
    "\n"
    "# Load swtorvr\\swtorvr_core.dll (the actual mod). 0 = loader only.\n"
    "LoadCore = 1\n"
    "\n"
    "# Reload the core automatically when swtorvr_core.dll changes on disk.\n"
    "AutoReload = 1\n"
    "\n"
    "# 3D scene resolution = game window size x SceneScale (1.0 .. 3.0). The UI/HUD keeps the window size.\n"
    "# Example: window 1920x1080 and SceneScale 2.0 -> scene 3840x2160 per eye. Needs a game restart.\n"
    "SceneScale = 1.0\n"
    "\n"
    "# Log level 0..4 (error, warn, info, debug, trace) for swtorvr\\swtorvr_loader.log\n"
    "LogLevel = 2\n"
    "\n"
    "# Log access violations etc. (first chance) with module + offset.\n"
    "CrashLog = 1\n";

static void Startup() {
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_gameDir = exe;
    g_gameDir = g_gameDir.substr(0, g_gameDir.find_last_of(L'\\') + 1);
    g_modDir = g_gameDir + L"swtorvr\\";
    CreateDirectoryW(g_modDir.c_str(), nullptr);
    std::wstring cfgPath = g_modDir + L"loader.txt";
    xrb::WriteDefaultConfig(cfgPath.c_str(), kDefaultConfig);
    xrb::Config cfg;
    cfg.Load(cfgPath.c_str());
    std::wstring logPath = g_modDir + L"swtorvr_loader.log";
    xrb::LogInit(logPath.c_str(), cfg.GetInt("LogLevel", 2));
    LOGI("SWTOR VR loader " LOADER_VERSION " (built " __DATE__ " " __TIME__ "), pid %lu, exe %ls", GetCurrentProcessId(), exe);
    std::string mode = cfg.GetStr("D3D9Mode", "ex");
    g_mode = mode == "ex" ? D3D9_EX : mode == "on12" ? D3D9_ON12 : D3D9_NATIVE;
    g_loadCore = cfg.GetBool("LoadCore", true);
    g_autoReload = cfg.GetBool("AutoReload", true);
    g_sceneScale = cfg.GetFloat("SceneScale", 1.0f);
    if (g_sceneScale < 1.0f) g_sceneScale = 1.0f;
    if (g_sceneScale > 3.0f) g_sceneScale = 3.0f;
    if (cfg.GetBool("CrashLog", true)) AddVectoredExceptionHandler(0, CrashVeh);
    LOGI("config: D3D9Mode=%s LoadCore=%d AutoReload=%d SceneScale=%.2f", mode.c_str(), g_loadCore, g_autoReload, g_sceneScale);
    InitializeCriticalSection(&g_coreCs);
    g_api.version = kLoaderApiVersion;
    g_api.size = sizeof(g_api);
    g_api.loaderModule = g_self;
    g_api.realD3D9 = Real();
    g_api.gameDir = g_gameDir.c_str();
    g_api.modDir = g_modDir.c_str();
    g_api.d3d9Mode = g_mode;
    g_api.Log = ApiLog;
    FILETIME ft;
    GetFileTime(g_modDir + L"swtorvr_core.dll", &g_coreTime, nullptr);
    (void)ft;
    CloseHandle(CreateThread(nullptr, 0, WatchThread, nullptr, 0, nullptr));
}

static void HookD3D(IDirect3D9* d3d, bool isEx) {
    if (!d3d) return;
    g_api.d3d = d3d;
    void* o;
    if ((o = PatchVtable(d3d, 16, (void*)Hk_CreateDevice))) o_CreateDevice = (PFN_CreateDevice)o;
    if (isEx && (o = PatchVtable(d3d, 20, (void*)Hk_CreateDeviceEx))) o_CreateDeviceEx = (PFN_CreateDeviceEx)o;
}

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdk) {
    Startup();
    void* caller = _ReturnAddress();
    HMODULE cm = nullptr;
    wchar_t cn[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)caller, &cm))
        GetModuleFileNameW(cm, cn, MAX_PATH);
    LOGI("Direct3DCreate9(%u) from %ls", sdk, wcsrchr(cn, L'\\') ? wcsrchr(cn, L'\\') + 1 : cn);
    IDirect3D9* d3d = nullptr;
    if (g_mode == D3D9_EX) {
        auto f = (HRESULT(WINAPI*)(UINT, IDirect3D9Ex**))RealProc("Direct3DCreate9Ex");
        IDirect3D9Ex* ex = nullptr;
        if (f && SUCCEEDED(f(sdk, &ex)) && ex) {
            d3d = ex;
            HookD3D(d3d, true);
            LOGI("created IDirect3D9Ex in place of IDirect3D9");
            return d3d;
        }
        LOGW("Direct3DCreate9Ex failed; falling back to native");
    } else if (g_mode == D3D9_ON12) {
        struct ARGS { BOOL Enable9On12; IUnknown* pD3D12Device; IUnknown* ppD3D12Queues[2]; UINT NumQueues; UINT NodeMask; };
        auto f = (IDirect3D9*(WINAPI*)(UINT, ARGS*, UINT))RealProc("Direct3DCreate9On12");
        ARGS a = { TRUE, nullptr, { nullptr, nullptr }, 0, 0 };
        if (f && (d3d = f(sdk, &a, 1))) { HookD3D(d3d, false); LOGI("created D3D9-on-12"); return d3d; }
        LOGW("Direct3DCreate9On12 failed; falling back to native");
    }
    auto f = (IDirect3D9*(WINAPI*)(UINT))RealProc("Direct3DCreate9");
    d3d = f ? f(sdk) : nullptr;
    HookD3D(d3d, false);
    return d3d;
}

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdk, IDirect3D9Ex** out) {
    Startup();
    LOGI("Direct3DCreate9Ex(%u)", sdk);
    auto f = (HRESULT(WINAPI*)(UINT, IDirect3D9Ex**))RealProc("Direct3DCreate9Ex");
    HRESULT hr = f ? f(sdk, out) : E_FAIL;
    if (SUCCEEDED(hr) && out && *out) HookD3D(*out, true);
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
    }
    return TRUE;
}
