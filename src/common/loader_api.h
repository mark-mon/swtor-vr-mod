// SWTOR VR - interface between the permanent loader (d3d9.dll proxy) and the hot-reloadable core DLL.
//
// The loader is installed once and never changes during a session. It owns:
//   - the d3d9 exports (forwarded to System32\d3d9.dll),
//   - device creation (capturing the device + present parameters),
//   - the Present / Reset interception (vtable patches),
//   - loading / unloading / reloading the core DLL.
// The core does everything else and must be able to remove itself completely in CoreShutdown().
//
// Reload happens on the render thread, inside the loader's Present hook, before the frame is presented:
//   core->CoreShutdown(); FreeLibrary(core); copy new core to run\; LoadLibrary; CoreInit(api, cb).
// So no core callback can be running on the render thread during a reload. Hooks the core installs on other
// threads must be guarded by the core itself (see core: HookGuard).
//
// Bump kLoaderApiVersion when this struct changes; the loader refuses a core built for another version.
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cstdint>

namespace svr {

constexpr uint32_t kLoaderApiVersion = 1;

enum D3D9Mode : int { D3D9_NATIVE = 0, D3D9_EX = 1, D3D9_ON12 = 2 };

struct LoaderApi {
    uint32_t version;                 // kLoaderApiVersion
    uint32_t size;                    // sizeof(LoaderApi)
    HMODULE loaderModule;
    HMODULE realD3D9;                 // System32\d3d9.dll
    const wchar_t* gameDir;           // folder of swtor.exe, with trailing backslash
    const wchar_t* modDir;            // gameDir + L"swtorvr\\"
    int d3d9Mode;                     // D3D9Mode actually in use
    IDirect3D9* d3d;                  // last IDirect3D9 the game created (may be null)
    IDirect3DDevice9* device;         // the game's device once created (null before)
    D3DPRESENT_PARAMETERS pp;         // last present parameters (CreateDevice / Reset)
    HWND focusWindow;
    uint32_t reloadCount;             // 0 for the first core load
    uint64_t presentCount;            // Presents seen by the loader since start
    void (*Log)(int level, const char* msg);   // writes to swtorvr_loader.log (0=E 1=W 2=I 3=D)
    // Scratch slots that survive core reloads (the loader never touches them). The core must not store
    // pointers into its own image here (code/data is gone after unload) - only heap or COM objects it
    // deliberately wants to keep (e.g. an XrBridge host connection that should survive reloads).
    void* persist[16];
};

struct CoreCallbacks {
    uint32_t size;                                                       // sizeof(CoreCallbacks)
    void (*OnDeviceCreated)(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS* pp);
    void (*OnPresentBegin)(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc);   // sc null for device Present
    void (*OnPresentEnd)(IDirect3DDevice9* dev, IDirect3DSwapChain9* sc, HRESULT hr);
    void (*OnPreReset)(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp);
    void (*OnPostReset)(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS* pp, HRESULT hr);
};

// Exported by the core DLL.
//
// Unloading is two-phase because the render thread can be INSIDE a core detour when Present (and so the reload)
// runs: the game calls Present from within DrawScene, which the core may hook. Freeing the core there returns into
// freed code. So:
//   1. SwtorVR_CoreShutdown(): disable every hook, release resources, stop threads. Must NOT wait for detours to
//      drain (the caller's own stack may hold one). After this no new call can enter the core.
//   2. The loader keeps the old module loaded ("retired") and asks SwtorVR_CoreCanUnload() at each later Present.
//      When it returns true (no thread inside a detour), the loader calls SwtorVR_CoreFinalize() (free MinHook
//      trampolines etc.) and FreeLibrary()s it. A new core can be loaded right after step 1.
typedef bool (__cdecl* PFN_CoreInit)(LoaderApi* api, CoreCallbacks* cb);   // false = refuse (loader unloads)
typedef void (__cdecl* PFN_CoreShutdown)();                                  // phase 1, never blocks on detours
typedef bool (__cdecl* PFN_CoreCanUnload)();                                 // optional; missing = always true
typedef void (__cdecl* PFN_CoreFinalize)();                                  // optional; phase 2, before FreeLibrary
typedef const char* (__cdecl* PFN_CoreVersion)();

} // namespace svr
