# Development notes (written by Claude while building the mod)

Reverse-engineering findings and design decisions, in the order they were made. Offsets refer to game build 7.9.1c.


Game build analysed: **7.9.1c** (swtor.exe 64-bit, 30,680,576 bytes, PE timestamp `0x6AC6A73E` = 2026-10-07;
RemoteRenderer.dll timestamp `0x6AC6A5A0`).

## Process / binaries

- One game process: `swtor\retailclient\swtor.exe`, started by `launcher.exe` (session token on the command line). Runs elevated.
- **Not packed**: normal MSVC 2017 sections (.text entropy 6.49), entry point in .text, no SteamStub/Denuvo, debug
  directory intact (`D:\auto\mmo1\branch\launch\builds\stage-retailclient-vs2017\bin\Release\swtor.pdb`). Static
  analysis on the file on disk works; no memory dump needed per patch.
- ~3,900 RTTI type descriptors (HeroEngine `bwa::` namespace, Scaleform GFx, Umbra/DPVS, SpaceCombat cameras).
- swtor.exe exports 25 names (Scaleform system + BugCatcher helpers).

## Renderer architecture: RemoteRenderer.dll (the key find)

- HeroEngine splits the game into "HeroEngine" (game logic, swtor.exe) and a **"RemoteRenderer" server that runs
  in-process** (`server start RemoteRenderer://test1@local::IpcConsole:1` in swtor_dual.icb). The game talks to it
  through `RemoteRendererClient.dll` (`bwa::RemoteRendererClientDLL::connect / drawScene / tick ...`).
- **`RemoteRenderer.dll` creates the D3D9 device** (imports `Direct3DCreate9`, `D3DPERF_SetOptions` from d3d9.dll and
  38 d3dx9_43 functions). swtor.exe also imports `Direct3DCreate9` (one import) - check which one actually creates
  the device (loader log prints the caller module).
- `RemoteRenderer.dll` **exports 224 named `bwa::RemoteRenderer` methods** - the full game->renderer API. Hooking
  by export name is update-proof (no scanning). Highlights:
  - Camera: `CameraNode_SetData(RenderProxyHandle&, const CameraNodeProxyData&)`, `SetActiveCameraNode`,
    `ClearSkysceneCamera`
  - Frame: `NotifyFrameBegin/Complete`, `NotifyRenderBegin/Complete`, `NotifyUpdateBegin/Complete`, `DrawScene`,
    `SubmitVisibilityList`
  - Cinematics / video: `EnterCinematic`, `SetCinematicLightData`, `PlayMovie`, `PresentMovie`, `StopMovie`,
    `GetMovieFrame` (Bink: binkw64.dll)
  - Cursor: `SetCursor`, `SetCursorPosition`, `SyncCursorMouseDown`, `MarkIgnoreMouseMoves`
  - Immediate mode (`IM_*`: BeginScene/EndScene, Draw*, SetRenderTarget, SetVertexShaderConstantF, SetTransform,
    PushRenderTarget...) - probably what Scaleform/UI and debug drawing use.
  - Resolution: `GraphicsOptions_SetResolutionRefreshRate`, `GraphicsOptions_EnumerateNextResolution`,
    `HandleDeviceReset`
  - Depth of field, colourblind post, `InCharacterSelect`, `SYNC_GetScreenshot`, `TakeScreenshot`.
- UI is **Scaleform GFx** (AS2 + AS3 classes present; `SF_GFx_RenderManager@BWA@Scaleform`).
- Mouse cursor: game uses `SetCursor` (Windows hardware cursor) -> invisible in VR, mod must draw it.

## Update-proofing strategy

1. Prefer RemoteRenderer exports by name (stable across patches as long as the API keeps its names).
2. Then RTTI vtables (class names) in swtor.exe / RemoteRenderer.dll.
3. Then byte signatures with wildcards; each must match exactly once and pass a runtime sanity check.
4. Offsets cache file keyed by module size + timestamp; hand-editable; scanner refills on a miss.

## Mod structure

- `d3d9.dll` loader (permanent): forwards d3d9 exports, captures device + present params, patches Present/Reset
  (device, Ex and swapchain vtables), loads `swtorvr\swtorvr_core.dll` via a copy in `swtorvr\run\`, hot-reloads it on
  file change / event `Local\SwtorVR_Reload` / Ctrl+Shift+F12; unload via `Local\SwtorVR_Unload` / Ctrl+Shift+F11.
  Config `swtorvr\loader.txt` (D3D9Mode native|ex|on12, LoadCore, AutoReload, LogLevel, CrashLog).
- `swtorvr_core.dll` (reloadable): everything else. Must remove all hooks in CoreShutdown; detours use HOOK_GUARD.

## Runtime facts (build 7.9.1c, test PC)

- Device: created by RemoteRenderer.dll on its thread, adapter 0, flags 0x40 (HARDWARE_VERTEXPROCESSING),
  1920x1080 X8R8G8B8 (fmt 21), D24S8 auto depth, windowed, swap DISCARD, interval IMMEDIATE. Direct3DCreate9 is
  also called by PlatformInfo.dll and swtor.exe (capability probes, no device).
- ONE thread does everything: DrawScene, CameraNode_SetData (2-4 calls/frame), NotifyFrame*/NotifyRender*,
  IM_DrawIndexedPrimitive (~40-100/frame, UI), SetCursor, and device Present (only IDirect3DDevice9::Present is
  used; no swapchain Present). ~93 fps at 1080p on the 3080 Ti.
- Present happens inside RemoteRenderer::NotifyRenderComplete (a probed export), so a reload always finds 1 active detour frame.
- **No D3DPOOL_MANAGED resources at all**: textures default 6.8k + systemmem 4.3k (staging), 163 dynamic, 35 RTs;
  VB/IB all default (about half dynamic). Locks: default-pool (dynamic) 470, systemmem 34k. -> D3D9Ex is viable.
- CameraNode_SetData copies 0x110 bytes (CameraNodeProxyData) to node+0x120 and sets node+0x464 = 1 (dirty).
  Node = [handle+8] when [handle+0x10] != 0. SetActiveCameraNode calls renderer vtable +0xC8 with the node.

## Hot reload (verified 2026-10-30)
- File-change reload, event reload, unload + reload all keep the game running; retired cores free ~0.5 s later.

## Input
- swtor.exe imports no DirectInput / raw input / GetAsyncKeyState; only GetKeyState. Keyboard arrives as window
  messages -> the mod can block keys for its menu by subclassing the game window (WM_KEY*/WM_CHAR) while open.

## VR output (verified 2026-10-30)
- D3D9Mode = ex works: whole game runs on IDirect3DDevice9Ex (94 fps). XrBridgeHost v6 + SLOT_MENU_MODE shows the
  back buffer as a world-locked theater quad. Copy cost 0.1 ms. Core hot reload restarts the host in ~0.5 s.
- Host dumps (config DebugHostDumpEverySec) land as swtorvr\host_<n>_menu.bmp.

## RemoteRenderer internals
- DrawScene(rcx=RR, rdx=timing struct): records timing, calls renderer vtable +0x38 (the real frame, which ends
  in Present), then sub_2dbc0.
- IM_DrawIndexedPrimitive: thin passthrough to IDirect3DDevice9::DrawIndexedPrimitive via renderer accessors.
- Node_SetData(handle, NodeProxyData*) -> node impl 0x79a00 copies fields (+0x80 handle, +0x90, +0x98..+0xa2
  flags, +0x40..+0x54 floats when +0x58 set, parent handle +0x60/+0x68).

## Frame structure (trace, 2026-10-30)
One game frame on the single render/game thread:
  NotifyFrameComplete / NotifyFrameBegin / NotifyUpdateBegin
  CameraNode_SetData x9 (main camera 3x: fov 0.873, 16:9; others: portrait/UI cams at the origin, 0.25/0.3 fov...)
  NotifyUpdateComplete
  NotifyRenderBegin  -> device Clear + BeginScene
  SubmitDebugDrawCommands, SubmitVisibilityList
  DrawScene          -> ONLY the 3D scene: shadow atlas 8192x4096 fmt 114 (R32F) cascades, main pass into the back
                        buffer (1920x1080 X8R8G8B8, D24S8), post chain (960x540, 480x270 ... bloom/DOF), ends with the
                        back buffer bound. ~730 draws.
  IM_DrawIndexedPrimitive xN (+IM_Clear)  -> the UI (Scaleform from swtor.exe) straight onto the bound back buffer
  NotifyRenderComplete -> EndScene + Present
=> Stereo = call DrawScene once per eye; HUD = bind our own RT after DrawScene so the UI renders into it.

## CameraNodeProxyData (0x110), decoded
  +0x00 Mat4 VIEW (world->camera, row-vector, camera looks -Z)   +0x40 Mat4 projection (RH, m[2][3] = -1)
  +0x80 Mat4 view*projection (far-plane variant, not used for the main view; maybe culling)
  +0xC0 float3 forward, right, up (world)    +0xE4 float3 camera world position
  +0xF0 ? (0.5775)  +0xF8 near (0.01)  +0xFC/+0x100 far (110)  +0x104 vertical FOV (0.8727 = 50 deg)
  Experiments: changing +0x00 changes the rendered view; changing only +0x80 changes nothing visible.
  Handle passed to CameraNode_SetData is a stack temporary; node = [handle+8] after the call. Active camera node from
  SetActiveCameraNode (rare: only on camera switches) -> kept in LoaderApi::persist[0] across core reloads.

## Stereo (2026-10-30)
- The renderer copies the active camera node's data (node+0x120) in RemoteRenderer+0x545a0, called from its
  begin-frame step (renderer vtable +0x28, which NotifyRenderBegin calls; also does Clear + BeginScene) and again
  inside DrawScene. Found with a hardware data breakpoint (core watch.cpp, DebugWatchCameraFrames).
- Working scheme: NotifyRenderBegin hook writes the LEFT eye into the node before the original runs; DrawScene hook:
  pass 1 -> copy back buffer to left eye; write RIGHT eye; call renderer vtable+0x28 again (StereoRefreshMode 1) so
  the renderer re-reads the node; pass 2 -> copy to right eye. The extra Clear is harmless (left already copied).
- Writing the node only between passes is NOT enough (the camera is read in begin-frame, not per DrawScene).
- vrmath InverseRigid had t' = -t*R instead of -t*R^T (eye offsets went into camera z). Fixed + unit-tested.
- Units: 1 SWTOR unit ~ 10 m (WorldScale default 0.1).

## Pacing, logs, menu (2026-10-30, live14-16)
- PaceToHeadset (default on, config only): after Present wait for the host's next headset frame (cap 2 periods + 2 ms, max 50 ms) while stereo frames flow. Fixed the 45/90 judder. Above ~SceneScale 1.85 the frame rate depends on what's in view (a GPU limit, not a DX9 copy wall).
- Debug page "Save logs for sending" copies core/loader/host logs (shared reads) to modDir\logs without quitting.
- Recenter view is the first Display row (F2, RecenterKey). World scale is shown x10. SkyFollowsHead, Stereo and PaceToHeadset default on. HudLayer is config only.

## Character-select UI flicker (fixed 2026-10-30, live17)
- The UI pass (Scaleform, after DrawScene) draws some panels through offscreen render targets, then calls SetRenderTarget(0, back buffer) to carry on. Our HUD capture had bound its own surface once, so everything after the re-bind went to the back buffer (already copied to the eyes). Which panels were affected changed per frame, e.g. Origin Stories and the PLAY buttons. The monitor looked fine.
- Measured at char select: about 1/3 of UI draws missed the HUD (~30 back-buffer re-binds/s).
- Fix: the SetRenderTarget hook (trace.cpp) calls Hud_FilterRT, which swaps the back buffer for the HUD surface while capturing (HudRedirectBackBuffer, default on). Afterwards >99% of UI draws land on the HUD; the rest go into the game's own offscreen targets, as intended. The hud stats line reports these counts every 10 s.

## Culling boost vs mouse picking (2026-10-31, live20)
- Widening the camera FOV in memory (persist / cull.cpp) also widens the projection the game uses for mouse picking: the hover area of world objects shrinks towards screen centre by the boost factor (boost 20+: nothing can be clicked).
- A 4-byte read watch on the FOV float (+0x1A0 in the game camera) finds 10 instructions on the game thread, each run every frame; none of them depends on the mouse. One of them is a store (+0x537f19: the game writing the FOV).
- Hit addresses are trap addresses (the instruction after the access). The reading instruction is found by decoding candidates ending there (hde64) and evaluating their memory operand with the registers at the trap (cullsite.cpp DecodeReader).
- Per-reader test (cullsite.cpp, DebugCullSiteScan = 2, DebugFixedHeadYawDeg = 60): memory keeps the game FOV, and an execute breakpoint (DR0-3, all threads) writes the wide value just before ONE reader and single-steps it, then writes the original back. Eye images per reader:
  - +0x537df3 (movss xmm1,[rsi+1A0h]; mulss ...): full culling gain, but it also feeds picking (taxi hover 4/33 points).
  - +0x53bf15 (movss xmm0,[rdi+1A0h]; movss [rsp+20h],xmm0; call): most of the culling gain, picking unaffected (31/33).
  - +0x535992 and +0x53c6f9: distort the sky / background (keep off).
  - The rest: no visible effect.
- Shipping: CullSites = auto finds +0x53bf15 by signature `F3 0F 11 44 24 28 F3 0F 10 87 A0 01 00 00 F3 0F 11 44 24 20 E8 ?? ?? ?? ?? 48 8B 0E 0F 57 DB 49 8B D6` (+6). It must match exactly once; there is a near-identical copy at +0x53c01d that differs after the call. If the signature isn't found, it falls back to widening everywhere.
- Draws per frame is a poor culling metric: a wider FOV lowers LOD, so fewer draws. Compare eye images instead.
- Test harness: sweep the mouse over a grid and read GetCursorInfo().hCursor. SWTOR switches the pointer when hovering an interactable, so this gives a click map.

## Game pointer in the headset (live20)
- GetCursorInfo().hCursor is converted to a texture whenever it changes: DrawIconEx over black and over white gives alpha = 1 - (white - black), with premultiplied colour from the black pass. The hotspot comes from GetIconInfo.

## Window modes (2026-10-31, live21)
- SceneScale only works in windowed mode. In fullscreen windowed the game resets at monitor size whatever GetClientRect says (user log: back buffer stayed 3840x2160, wanted 9600x5400, reset every retry = flashing). sceneres now gives up after one reset at the wrong size, until the window size changes. The scaled size is capped at the device's MaxTextureWidth/Height.
- ClipCursorToGame (default on): ClipCursor to the client rect while the game is in front and the XR host is up, re-checked every 250 ms. A narrower clip set by the game (camera drag) is left alone; released on focus loss and unload.
- Reported, not reproduced: in windowed mode the window sometimes collapses to minimum size (title bar only) after a "resolution change", and VR does not come back. SceneScale changes 1.0 to 2.5 on the dev PC did not trigger it.

## MSAA and the HUD (2026-10-31, v1.0.1)
- With in-game anti-aliasing High / Very High the game uses MSAA (`ms 4` in the loader's CreateDevice line). The HUD capture binds a non-multisampled render target while the game's depth-stencil stays multisampled; D3D9 drops draws with that mismatch whenever depth or stencil is used. Scaleform masks the conversation wheel and the answer list with stencil, so in VR only the subtitle showed (the answers still worked when clicked blind).
- Fix: during the capture the UI gets its own D24S8 depth-stencil (HUD size, no MSAA, cleared at capture start). SetDepthStencilSurface(game's) calls during the capture are redirected to it (trace.cpp Hk_SetDS -> Hud_FilterDS). The game's buffer is put back at the end. Only when the game's buffer is multisampled or smaller. `HudOwnDepth = 0` turns it off.
- Test aids: `DebugForceMSAA = 4` forces multisampling on the next device reset; `TraceCinematics = 1` traces one frame 1.5 s and 8 s after a conversation starts.
