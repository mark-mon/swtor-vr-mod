// Shared core declarations.
#pragma once
#include <atomic>
#include "common/loader_api.h"   // includes d3d9.h
namespace xrb { class Config; struct IpcPose; struct IpcFov; }

namespace svr {
extern LoaderApi* g_api;
void GameUiSize(unsigned* w, unsigned* h);
// sceneres.cpp
void SceneRes_LoadSettings(const xrb::Config& c);
void SceneRes_Install();
void SceneRes_Uninstall();
void SceneRes_NoteReset();
void SceneRes_OnFrameBegin(void* rr);
bool SceneRes_RealClientSize(unsigned* w, unsigned* h);
extern std::atomic<int> g_hookDepth;          // threads currently inside a core detour
extern std::atomic<bool> g_shuttingDown;

// Put at the top of every detour that game threads can enter. Shutdown waits for the count to reach zero
// before the core DLL is unloaded, so no thread can return into freed core code.
struct HookGuardScope {
    HookGuardScope() { g_hookDepth.fetch_add(1, std::memory_order_acq_rel); }
    ~HookGuardScope() { g_hookDepth.fetch_sub(1, std::memory_order_acq_rel); }
};

void ResStats_Install(IDirect3DDevice9* dev);
void ResStats_Log();
void ResStats_Reset();
bool Camera_Install(HMODULE rr);
struct CamSettings { int debugPatchMode = 0; float debugYawDeg = 0.f; };
void Camera_LoadSettings(const xrb::Config& c);
void* Camera_ActiveNode();
// trace.cpp
bool Trace_Active();
void Trace_Event(const char* fmt, ...);
void Trace_Enter(const char* name);
void Trace_Leave(const char* name);
void Trace_Request(int frames);
void Trace_FrameBoundary();
void Trace_InstallDeviceHooks(IDirect3DDevice9* dev);
void Trace_LoadSettings(const xrb::Config& c);
void Trace_LogShadowStats();

struct VrSettings { bool enabled = true; float theaterDistance = 2.5f, theaterWidth = 3.2f; int hostLogLevel = 2; int debugHostDumpEverySec = 0; float eyeScale = 1.0f; int theaterAfterMs = 500; bool paceToHeadset = true; bool clipCursor = true; };
const VrSettings& Vr_Settings();
void Vr_LoadSettings(const xrb::Config& c);
void Vr_OnDevice(IDirect3DDevice9* dev, const D3DPRESENT_PARAMETERS* pp);
void Vr_OnPresent(IDirect3DDevice9* dev);
void Vr_OnPreReset();
void Vr_OnPostReset(const D3DPRESENT_PARAMETERS* pp);
void Vr_LogStats();
bool Vr_FlatMode();
void Vr_Recenter();
void Stereo_SetWorldScale(float w);
void Vr_OnPresentEnd();
void Vr_SetPacing(bool on);
void Vr_ToggleFlat();
// sharpen.cpp
void Sharpen_LoadSettings(const xrb::Config& c);
bool Sharpen_Enabled();
bool Sharpen_CopyBackBuffer(IDirect3DDevice9* dev, IDirect3DSurface9* dst, const RECT& r);
void Sharpen_Release();
void Sharpen_Set(float strength, float clampV, float radius);
// cull.cpp
void Cull_LoadSettings(const xrb::Config& c);
void Cull_Tick();
void Cull_Frame();
void Cull_SetWiden(float w);
void Cull_SetAspect(float a);
bool Log_SaveSnapshot();
void Log_CleanOldCopies();   // fresh game start: remove the dated copies from earlier runs
void LogSave_LoadSettings(const xrb::Config& c);
bool Stereo_GameProjection(float* p00, float* p11, float* nearZ);
float Stereo_GameFov();
bool Stereo_GameNearFar(float* n, float* f);
void Cull_Shutdown();
void CullSites_LoadSettings(const xrb::Config& c);
void CullSites_Frame(float* fovAddr, float orig, float wide);
bool CullSites_MemoryWide();
void CullSites_Pause();
void CullSites_Shutdown();
uint32_t Trace_LastFrameDraws();
void Vr_RequestDump(uint32_t id);
void CrashLog_Install();
void CrashLog_Remove();
void VidMem_Log();
void VidMem_Release();
void FovFind_LoadSettings(const xrb::Config& c);
void FovFind_Frame();
// menu.cpp
void Menu_LoadSettings(const xrb::Config& c);
bool Menu_OnPresent(IDirect3DDevice9* dev, IDirect3DSurface9* target, const RECT& rect);
bool Menu_IsOpen();
bool Menu_WantsFrame();
void Menu_OnPreReset();
void Menu_Shutdown();
void Vr_Shutdown();
// hud.cpp
struct HudSettings { bool enabled = true; float distance = 1.4f, width = 1.6f, offsetY = -0.05f; bool followHead = false; bool drawCursor = true; bool gamePointer = true; int cursorSize = 32; int alphaMode = 1; int debugDumpAfterSec = 0; };
void Hud_LoadSettings(const xrb::Config& c);
const HudSettings& Hud_Settings();
bool Hud_Install(IDirect3DDevice9* dev);
void Hud_BeginCapture(IDirect3DDevice9* dev);
bool Hud_EndCapture(IDirect3DDevice9* dev);
bool Hud_Capturing();
IDirect3DSurface9* Hud_Surface();
void Hud_Release();
void Hud_LogStats();
IDirect3DSurface9* Hud_FilterRT(DWORD index, IDirect3DSurface9* s);   // SetRenderTarget while capturing
void Hud_OnDraw();
IDirect3DSurface9* Hud_FilterDS(IDirect3DSurface9* s);
void Hud_DrawCursorOnto(IDirect3DDevice9* dev, IDirect3DSurface9* target, const RECT& rect);
bool Vr_StereoReady();
bool Vr_BeginStereoFrame();
void Vr_CaptureEye(int eye);
// stereo.cpp
struct StereoSettings { bool enabled = true; float worldScale = 1.0f; bool headTracking = true; int fieldMask = 7; bool setProjection = true; int writeMode = 1; float debugSeparation = 0.f; float debugFakeYawDeg = 0.f; float debugFixedYawDeg = 0.f; int refreshMode = 1; bool followers = true; };
bool Stereo_FilterActiveCamera(const void* data, uint8_t* out);
void Stereo_LoadSettings(const xrb::Config& c);
const StereoSettings& Stereo_Settings();
bool Stereo_Install(HMODULE rr);
void Stereo_OnActiveCamera(void* node, const void* data);
void Stereo_OnFollowerCamera(void* node, const void* data);
void Stereo_LogStats();
int Stereo_Pass();
void Watch_Start(void* node);
void Watch_Stop();
void Watch_StartWrite(void* addr, const char* what);
void Watch_StartAccess(void* addr, const char* what);
bool Watch_Active();
void Watch_ReportCounts(const char* label);
const xrb::IpcPose* Stereo_Poses();
const xrb::IpcFov* Stereo_Fovs();
int64_t Stereo_PoseTime();

// resolver.cpp - exports by name, signature scans (exactly one match), offsets.txt cache per module build
void* Resolver_Sig(const char* name, const char* module, const char* pattern, int offset = 0, char resolve = 0, int insnEnd = 4);
void* Resolver_Export(const char* module, const char* exportName);
void Resolver_SaveCache();
void Camera_Tick();
} // namespace svr
#define HOOK_GUARD ::svr::HookGuardScope _hookGuard
