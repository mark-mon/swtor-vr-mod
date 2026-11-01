// SWTOR VR core - the hot-reloadable part of the mod (loaded by the d3d9.dll loader, see loader_api.h).
//
// Rules for everything in the core:
//   * CoreShutdown() must leave the process as if the core had never been loaded: all MinHook hooks removed,
//     all threads joined, all D3D resources released, no callbacks registered anywhere.
//   * Every detour on a game thread uses HOOK_GUARD so shutdown can wait until no thread is inside core code.
//   * Game addresses come from the resolver (renderer exports by name, signature scans) - never hard-coded.
#include <windows.h>
#include <d3d9.h>
#include <psapi.h>
#include <cstdio>
#include <atomic>
#include <string>
#include <utility>
#include "MinHook.h"
#include "common/loader_api.h"
#include "common/log.h"
#include "common/config.h"
#include "core.h"

#ifndef SVR_BUILD_ID
#define SVR_BUILD_ID "dev"
#endif
#define CORE_VERSION "1.0.2+" SVR_BUILD_ID

namespace svr {

LoaderApi* g_api = nullptr;

// Size the game laid its UI out for. With loader SceneScale the back buffer is larger than this.
void GameUiSize(unsigned* w, unsigned* h) {
    typedef void(__cdecl * PFN)(unsigned*, unsigned*);
    static PFN fn = (PFN)GetProcAddress(g_api->loaderModule, "SwtorVR_LoaderGameSize");   // loader >= 0.3
    unsigned gw = 0, gh = 0;
    if (SceneRes_RealClientSize(&gw, &gh)) { *w = gw; *h = gh; return; }   // the real window = UI layout size
    if (fn) fn(&gw, &gh);
    if (!gw || !gh) { gw = g_api->pp.BackBufferWidth; gh = g_api->pp.BackBufferHeight; }
    *w = gw; *h = gh;
}

static const char* kDefaultConfig =
    "# SWTOR VR settings (swtorvr\\config.txt). Re-read whenever the core (re)loads.\n"
    "\n"
    "# VR output through XrBridgeHost.exe (needs D3D9Mode = ex in loader.txt). 0 = off.\n"
    "VR = 1\n"
    "\n"
    "# Theater screen: distance and width in meters.\n"
    "TheaterDistance = 2.5\n"
    "TheaterWidth = 3.2\n"
    "\n"
    "# XrBridgeHost log level 0..4 (swtorvr\\xrhost.log)\n"
    "HostLogLevel = 2\n"
    "\n"
    "# Native stereo: the scene is rendered once per eye with the headset's eye poses. 0 = theater screen.\n"
    "Stereo = 1\n"
    "# Game units per meter (SWTOR: 1 unit = about 10 m) for head movement and IPD.\n"
    "WorldScale = 0.1\n"
    "# Camera follows the headset (0 = eyes only, no head rotation).\n"
    "HeadTracking = 1\n"
    "# Use the headset's per-eye field of view.\n"
    "HeadsetProjection = 1\n"
    "# 3D scene resolution = window size x SceneScale (1.0 .. 3.0). The UI / HUD keeps the window size.\n"
    "# Applies live (the renderer resets to the new size). Keep loader.txt SceneScale at 1.0.\n"
    "SceneScale = 1.0\n"
    "# Eye texture size relative to the headset's recommended size.\n"
    "EyeTextureScale = 1.0\n"
    "# Background sky cameras follow head rotation (sky fix).\n"
    "SkyFollowsHead = 1\n"
    "# The game renders one frame per headset frame (90, or 45 when SteamVR throttles).\n"
    "PaceToHeadset = 1\n"
    "# Culling: the game's own camera FOV is widened by this so objects beside you are drawn (30 = ~175 deg).\n"
    "CullWiden = 30\n"
    "# Which game code sees the widened FOV: auto = only the culling code found by signature (mouse picking keeps\n"
    "# working); off = widen everywhere (old behaviour: clicking world objects fails at a high boost).\n"
    "CullSites = auto\n"
    "# Culling boost widens sideways only (keeps object sizes, so nothing pops in straight ahead); 0 = widen the FOV.\n"
    "CullSplit = 1\n"
    "# Extra vertical room for the culling view when CullSplit = 1 (for looking up / down), as a tangent factor.\n"
    "CullVertBoost = 1.5\n"
    "# Draw the game's own mouse pointer (talk, loot, taxi, ...) in the headset; 0 = a plain arrow.\n"
    "GamePointer = 1\n"
    "# Luma sharpen (ReShade LumaSharpen): strength (0 = off), clamp, radius.\n"
    "Sharpen = 0\n"
    "SharpenClamp = 0.035\n"
    "SharpenRadius = 1.0\n"
    "\n"
    "# Keys (Insert, F1..F24, a letter, Home, End, Pause, or a 0x.. virtual-key code; none = off)\n"
    "MenuKey = Insert\n"
    "RecenterKey = F2\n"
    "FlatToggleKey = F1\n"
    "FlatToggleKeyToGame = 0\n";
std::atomic<int> g_hookDepth{0};
std::atomic<bool> g_shuttingDown{false};
static bool g_mhInit = false;
static int g_traceFramesCfg = 0, g_watchFramesCfg = 0;
// Diagnostics for conversations (TraceCinematics = 1, off by default; a few per game session): when the game enters a
// cinematic / conversation, one frame is traced 1.5 s and 8 s later (the dialog wheel appears after the NPC speaks).
static int g_traceCinematics = 0;
static volatile LONG g_cinematicEntered = 0;
static int g_cinematicsTraced = 0;

// --- renderer API probe: resolve exports by name and count calls (thread + rate) ---------------------------------
struct Probe {
    const char* shortName;
    const char* exportName;   // mangled name in RemoteRenderer.dll
    void* target = nullptr;
    void* orig = nullptr;
    std::atomic<uint64_t> calls{0};
    std::atomic<DWORD> lastThread{0};
};

// Generic 4-integer-argument passthrough is not safe for arbitrary signatures, so each probe has its own detour.
// All probed functions are RemoteRenderer member functions: rcx = this, then up to 3 more args in rdx/r8/r9 plus
// stack args. A detour declared with 8 pointer-size args and forwarding all of them keeps stack args intact as
// long as the callee does not take > 8 args and no float args sit in the first four slots (xmm). The probes below
// were picked for integer/pointer signatures (from the mangled names).
typedef uintptr_t(__fastcall* Fn8)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

static Probe g_probes[] = {
    {"NotifyFrameBegin", nullptr},
    {"NotifyFrameComplete", nullptr},
    {"NotifyRenderComplete", nullptr},
    {"EnterCinematic", nullptr},
    {"PlayMovie", nullptr},
    {"PresentMovie", nullptr},
    {"StopMovie", nullptr},
    {"SetCursor", nullptr},
    {"SetCursorPosition", nullptr},
    {"IM_BeginScene", nullptr},
    {"IM_EndScene", nullptr},
    {"IM_DrawIndexedPrimitive", nullptr},
    {"IM_DrawPrimitiveUP", nullptr},
    {"InCharacterSelect", nullptr},
    {"SubmitVisibilityList", nullptr},
    {"IM_SetRenderTarget", nullptr},
    {"IM_PushRenderTarget", nullptr},
    {"IM_PopRenderTarget", nullptr},
    {"IM_Clear", nullptr},
    {"IM_SetViewport", nullptr},
    {"IM_SetVertexShaderConstantF", nullptr},
    {"UpdateMiniMap", nullptr},
    {"SubmitDebugDrawCommands", nullptr},
    {"RenderColorblindPostprocess", nullptr},
    {"NotifyUpdateBegin", nullptr},
    {"NotifyUpdateComplete", nullptr},
};
constexpr int kProbes = sizeof(g_probes) / sizeof(g_probes[0]);

template <int I>
static uintptr_t __fastcall ProbeDetour(uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d, uintptr_t e, uintptr_t f,
                                        uintptr_t g, uintptr_t h) {
    HOOK_GUARD;
    g_probes[I].calls++;
    g_probes[I].lastThread = GetCurrentThreadId();
    if (I == 3) InterlockedExchange(&g_cinematicEntered, 1);   // EnterCinematic
    if (!Trace_Active()) return ((Fn8)g_probes[I].orig)(a, b, c, d, e, f, g, h);
    Trace_Enter(g_probes[I].shortName);
    uintptr_t r = ((Fn8)g_probes[I].orig)(a, b, c, d, e, f, g, h);
    Trace_Leave(g_probes[I].shortName);
    return r;
}
template <int... Is>
static void* DetourFor(int i, std::integer_sequence<int, Is...>) {
    static void* table[] = {(void*)&ProbeDetour<Is>...};
    return table[i];
}

static void InstallProbes(HMODULE rr) {
    // Resolve by the short (demangled) method name so the exact parameter mangling does not matter.
    auto* dos = (IMAGE_DOS_HEADER*)rr;
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)rr + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    auto* ex = (IMAGE_EXPORT_DIRECTORY*)((uint8_t*)rr + dir.VirtualAddress);
    auto* names = (DWORD*)((uint8_t*)rr + ex->AddressOfNames);
    auto* ords = (WORD*)((uint8_t*)rr + ex->AddressOfNameOrdinals);
    auto* funcs = (DWORD*)((uint8_t*)rr + ex->AddressOfFunctions);
    int found = 0;
    for (int p = 0; p < kProbes; p++) {
        std::string want = std::string("?") + g_probes[p].shortName + "@RemoteRenderer@bwa@@";
        int hits = 0;
        for (DWORD i = 0; i < ex->NumberOfNames; i++) {
            const char* n = (const char*)rr + names[i];
            if (strncmp(n, want.c_str(), want.size()) == 0) {
                if (!hits) g_probes[p].target = (uint8_t*)rr + funcs[ords[i]];
                hits++;
            }
        }
        if (hits != 1) {
            LOGW("probe %s: %d exports match - skipped", g_probes[p].shortName, hits);
            g_probes[p].target = nullptr;
            continue;
        }
        MH_STATUS s = MH_CreateHook(g_probes[p].target, DetourFor(p, std::make_integer_sequence<int, kProbes>{}), &g_probes[p].orig);
        if (s == MH_OK) s = MH_EnableHook(g_probes[p].target);
        if (s != MH_OK) { LOGW("probe %s: hook failed (%s)", g_probes[p].shortName, MH_StatusToString(s)); g_probes[p].target = nullptr; continue; }
        LOGI("probe %-24s RemoteRenderer+0x%llx", g_probes[p].shortName, (unsigned long long)((uint8_t*)g_probes[p].target - (uint8_t*)rr));
        found++;
    }
    LOGI("probes installed: %d / %d", found, kProbes);
}

static void LogProbeStats() {
    std::string line;
    char buf[160];
    for (int p = 0; p < kProbes; p++) {
        if (!g_probes[p].target) continue;
        uint64_t n = g_probes[p].calls.exchange(0);
        snprintf(buf, sizeof(buf), " %s=%llu(t%lu)", g_probes[p].shortName, (unsigned long long)n, g_probes[p].lastThread.load());
        line += buf;
    }
    LOGI("calls/10s:%s", line.c_str());
}

// --- callbacks from the loader ------------------------------------------------------------------------------------
static uint64_t g_frames = 0;
static DWORD g_lastStats = 0;
static DWORD g_renderThread = 0;

static void OnDeviceCreated(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS* pp) {
    LOGI("device %p: back buffer %ux%u, windowed %d", dev, pp->BackBufferWidth, pp->BackBufferHeight, pp->Windowed);
    ResStats_Install(dev);
    Trace_InstallDeviceHooks(dev);
    Hud_Install(dev);
    static bool sceneResDone = false;
    if (!sceneResDone) { sceneResDone = true; SceneRes_Install(); }
    Vr_OnDevice(dev, pp);
}
static void OnPresentBegin(IDirect3DDevice9* dev, IDirect3DSwapChain9*) {
    Trace_FrameBoundary();
    if (!g_renderThread) { g_renderThread = GetCurrentThreadId(); LOGI("first Present on thread %lu", g_renderThread); }
    static uint64_t frameNo = 0;
    if (++frameNo == 600 && g_traceFramesCfg > 0) Trace_Request(g_traceFramesCfg);
    {
        static uint64_t cineAt = 0;
        if (InterlockedExchange(&g_cinematicEntered, 0) && g_traceCinematics && g_cinematicsTraced < 3) {
            cineAt = frameNo; g_cinematicsTraced++;
            LOGI("diag: cinematic / conversation started - tracing a frame in 1.5 s and 8 s");
        }
        if (cineAt && (frameNo == cineAt + 135 || frameNo == cineAt + 720)) { Trace_Request(1); Hud_LogStats(); }
        if (cineAt && frameNo > cineAt + 720) cineAt = 0;
    }
    if (g_watchFramesCfg > 0 && frameNo == 600) Watch_Start(Camera_ActiveNode());
    if (g_watchFramesCfg > 0 && frameNo == 600 + (uint64_t)g_watchFramesCfg) Watch_Stop();
    Cull_Frame();
    FovFind_Frame();
    Vr_OnPresent(dev);
    g_frames++;
    DWORD now = GetTickCount();
    if (now - g_lastStats >= 10000) {
        if (g_lastStats) LOGI("frames/10s: %llu (%.1f fps), present thread %lu", (unsigned long long)g_frames, g_frames / ((now - g_lastStats) / 1000.0), g_renderThread);
        LogProbeStats();
        ResStats_Log();
        VidMem_Log();
        Camera_Tick();
        Cull_Tick();
        Vr_LogStats();
        Stereo_LogStats();
        Trace_LogShadowStats();
        Hud_LogStats();
        g_frames = 0;
        g_lastStats = now;
    }
}
static void OnPresentEnd(IDirect3DDevice9*, IDirect3DSwapChain9*, HRESULT) { Vr_OnPresentEnd(); }
static void LogCallers(const char* what) {
    void* fr[24];
    USHORT n = RtlCaptureStackBackTrace(1, 24, fr, nullptr);
    char line[1400]; int k = snprintf(line, sizeof(line), "%s callers:", what);
    for (USHORT i = 0; i < n && k < (int)sizeof(line) - 80; i++) {
        HMODULE m = nullptr; char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fr[i], &m)) {
            GetModuleFileNameA(m, name, MAX_PATH);
            const char* b = strrchr(name, '\\'); if (b) memmove(name, b + 1, strlen(b));
        }
        k += snprintf(line + k, sizeof(line) - k, " %s+0x%llx", name, (unsigned long long)((uint8_t*)fr[i] - (uint8_t*)m));
    }
    LOGI("%s", line);
}
static int g_resetLogs = 0;
static int g_debugForceMsaa = 0;   // test aid: DebugForceMSAA = 2/4/8 forces multisampling on the next device reset
static void OnPreReset(IDirect3DDevice9*, D3DPRESENT_PARAMETERS* pp) {
    if (g_debugForceMsaa >= 2 && pp->SwapEffect == D3DSWAPEFFECT_DISCARD) { pp->MultiSampleType = (D3DMULTISAMPLE_TYPE)g_debugForceMsaa; pp->MultiSampleQuality = 0; }
    LOGI("pre-Reset %ux%u ms %d", pp->BackBufferWidth, pp->BackBufferHeight, (int)pp->MultiSampleType);
    SceneRes_NoteReset();
    if (g_resetLogs++ < 6) LogCallers("Reset");
    Vr_OnPreReset();
}
static void OnPostReset(IDirect3DDevice9*, const D3DPRESENT_PARAMETERS* pp, HRESULT hr) { LOGI("post-Reset 0x%08lx", hr); if (SUCCEEDED(hr)) Vr_OnPostReset(pp); }

} // namespace svr

using namespace svr;

extern "C" __declspec(dllexport) const char* __cdecl SwtorVR_CoreVersion() { return CORE_VERSION; }

extern "C" __declspec(dllexport) bool __cdecl SwtorVR_CoreInit(LoaderApi* api, CoreCallbacks* cb) {
    if (!api || api->version != kLoaderApiVersion || api->size != sizeof(LoaderApi)) return false;
    g_api = api;
    g_shuttingDown = false;
    std::wstring log = std::wstring(api->modDir) + L"swtorvr_core.log";
    xrb::LogInit(log.c_str(), 2, api->reloadCount > 0);   // keep history across reloads
    LOGI("SWTOR VR core %s  (load #%u, loader present count %llu)", CORE_VERSION, api->reloadCount,
         (unsigned long long)api->presentCount);
    CrashLog_Install();
    {
        std::wstring cfgPath = std::wstring(api->modDir) + L"config.txt";
        xrb::WriteDefaultConfig(cfgPath.c_str(), kDefaultConfig);
        xrb::Config cfg;
        cfg.Load(cfgPath.c_str());
        Vr_LoadSettings(cfg);
        Camera_LoadSettings(cfg);
        Stereo_LoadSettings(cfg);
        Hud_LoadSettings(cfg);
        g_debugForceMsaa = cfg.GetInt("DebugForceMSAA", 0);
        SceneRes_LoadSettings(cfg);
        Trace_LoadSettings(cfg);
        Menu_LoadSettings(cfg);
        Sharpen_LoadSettings(cfg);
        Cull_LoadSettings(cfg);
        LogSave_LoadSettings(cfg);
        if (api->reloadCount == 0) Log_CleanOldCopies();
        FovFind_LoadSettings(cfg);
        {   // make the key settings discoverable in an existing config.txt
            std::string add;
            if (cfg.GetStr("MenuKey", "\x01") == "\x01") add += "\r\n# Key that opens / closes the in-headset menu (Insert, F1..F24, a letter, Home, End, Pause, or 0x.. virtual-key code)\r\nMenuKey = Insert\r\n";
            if (cfg.GetStr("RecenterKey", "\x01") == "\x01") add += "\r\n# Key that recenters the VR view\r\nRecenterKey = F2\r\n";
            if (cfg.GetStr("FlatToggleKey", "\x01") == "\x01") add += "\r\n# Key that switches between VR and the flat theater screen (none = off)\r\nFlatToggleKey = F1\r\n# 1 = the game also receives that key\r\nFlatToggleKeyToGame = 0\r\n";
            if (!add.empty()) {
                FILE* f = _wfopen(cfgPath.c_str(), L"ab");
                if (f) { fwrite(add.data(), 1, add.size(), f); fclose(f); }
            }
        }
        g_traceFramesCfg = cfg.GetInt("DebugTraceFrames", 0);
        g_traceCinematics = cfg.GetInt("TraceCinematics", 0);
        g_watchFramesCfg = cfg.GetInt("DebugWatchCameraFrames", 0);
        LOGI("config: VR=%d TheaterDistance=%.2f TheaterWidth=%.2f", Vr_Settings().enabled, Vr_Settings().theaterDistance, Vr_Settings().theaterWidth);
    }
    if (MH_Initialize() != MH_OK) { LOGE("MH_Initialize failed"); return false; }
    g_mhInit = true;

    HMODULE exe = GetModuleHandleW(nullptr), rr = GetModuleHandleW(L"RemoteRenderer.dll");
    MODULEINFO mi = {};
    GetModuleInformation(GetCurrentProcess(), exe, &mi, sizeof(mi));
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    LOGI("swtor.exe base %p size 0x%lx timestamp 0x%08lx", exe, mi.SizeOfImage, nt->FileHeader.TimeDateStamp);
    if (rr) {
        auto* rnt = (IMAGE_NT_HEADERS*)((uint8_t*)rr + ((IMAGE_DOS_HEADER*)rr)->e_lfanew);
        LOGI("RemoteRenderer.dll base %p timestamp 0x%08lx", rr, rnt->FileHeader.TimeDateStamp);
        InstallProbes(rr);
        Camera_Install(rr);
        Stereo_Install(rr);
    } else {
        LOGW("RemoteRenderer.dll not loaded");
    }

    cb->OnDeviceCreated = OnDeviceCreated;
    cb->OnPresentBegin = OnPresentBegin;
    cb->OnPresentEnd = OnPresentEnd;
    cb->OnPreReset = OnPreReset;
    cb->OnPostReset = OnPostReset;
    g_lastStats = GetTickCount();
    return true;
}

// Phase 1 (see loader_api.h): disable all hooks; never wait for detours here - the calling thread itself may be
// inside one (Present is called from within DrawScene).
extern "C" __declspec(dllexport) void __cdecl SwtorVR_CoreShutdown() {
    LOGI("core shutdown (phase 1): disabling hooks, %d detour frames active", g_hookDepth.load());
    g_shuttingDown = true;
    if (g_mhInit) MH_DisableHook(MH_ALL_HOOKS);
    Cull_Shutdown();
    VidMem_Release();
    Watch_Stop();
    CrashLog_Remove();
    SceneRes_Uninstall();
    Vr_Shutdown();
    LOGI("core shutdown (phase 1) done; closing log (the next core appends to it)");
    xrb::LogClose();
}

// Loader polls this each frame after phase 1; true once no thread is inside a core detour.
extern "C" __declspec(dllexport) bool __cdecl SwtorVR_CoreCanUnload() { return g_hookDepth.load(std::memory_order_acquire) == 0; }

// Phase 2: right before FreeLibrary.
extern "C" __declspec(dllexport) void __cdecl SwtorVR_CoreFinalize() {
    if (g_mhInit) { MH_Uninitialize(); g_mhInit = false; }
    ResStats_Reset();
    if (g_api && g_api->Log) g_api->Log(2, "core finalized (phase 2)");
}

BOOL WINAPI DllMain(HINSTANCE, DWORD, LPVOID) { return TRUE; }
