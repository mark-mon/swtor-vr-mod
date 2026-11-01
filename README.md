# SWTOR VR

A VR mod for Star Wars: The Old Republic: full stereo 3D with head tracking, the game's HUD on its own panel, and a theater screen for cutscenes and loading screens. It runs through SteamVR or any other OpenXR runtime.

> **This mod may be against the game's ToS. SWTOR seems to take a dont flaunt it dont ban it approach, but account bans are always possible, use at your own risk.**

**Download, install and usage: see [Releases](https://github.com/mark-mon/swtor-vr-mod/releases).** The release notes cover the required game settings (windowed mode, 90 fps, VSync off) and the keys.

## How it works

SWTOR is a 64-bit Direct3D 9 game. Its renderer lives in `RemoteRenderer.dll`, which exports more than 200 named `bwa::RemoteRenderer` functions; the mod hooks those by name, so most game patches don't break it.

| Part | What it does |
|---|---|
| `d3d9.dll` (`src/loader`) | Proxy loader next to `swtor.exe`. Creates the device as D3D9Ex (needed to share frames), and loads and hot-reloads the core from `swtorvr\` (two-phase unload, so the game never runs code that was unloaded). |
| `swtorvr_core.dll` (`src/core`) | The mod. Renders `DrawScene` twice per frame with per-eye cameras and projections. Captures the Scaleform UI into a separate HUD texture. Sends eyes + HUD to the host through a ring of shared D3D9Ex textures. |
| `XrBridgeHost.exe` (`xrbridge/`) | Separate 64-bit OpenXR process: shows the shared textures as a projection layer plus a HUD quad, and returns head poses. |
| `src/vrkit` | Keyboard/gamepad ImGui menu drawn into the HUD. |

Notable pieces:

- **Render scale** (`sceneres.cpp`): only the renderer sees a larger window, so the 3D scene renders above window size while the UI keeps its layout.
- **Culling boost** (`cull.cpp`, `cullsite.cpp`): the game's visibility culling uses the flat camera's FOV, so objects vanish when you turn your head. The camera FOV in memory stays as the game set it. An execute breakpoint on the call that builds the culling view (found by signature at runtime) widens only its aspect ratio, plus a little height. Objects keep their on-screen size for the game's "too small to draw" check, so nothing pops in ahead. Mouse picking and the sky keep the real FOV, so clicking world objects still works.
- **HUD capture** (`hud.cpp`): redirects everything drawn after the 3D scene into a texture with forced alpha. Back-buffer re-binds in the middle of the UI pass are redirected too.

`docs/CLAUDE_NOTES.md` has the full reverse-engineering notes (renderer API, frame structure, camera data layout, what was tried and why).

## Building

`scripts/build.sh [loader|core|all]` builds `dist/d3d9.dll` and `dist/swtorvr_core.dll`. The scripts cross-compile on Linux with clang-cl and lld-link against the MSVC STL and the Windows SDK; the toolchain wrapper path is set by `TC`. With Visual Studio, build the same file lists as x64 DLLs: loader with `src/loader/d3d9.def`, core with `/EHa`. Both link `user32`, `gdi32`, `d3d9` and `psapi`.

`xrbridge/` contains the host's source and its own build scripts (`build_win_host.cmd` for Windows, `build.sh`).

## Hot reload (for development)

Overwrite `swtorvr\swtorvr_core.dll` while the game runs and the loader reloads it, or press Ctrl+Shift+F12. Ctrl+Shift+F11 unloads it. Logs go to `swtorvr\swtorvr_core.log` and `swtorvr\swtorvr_loader.log`.

## License

GPL-3.0 (see `LICENSE`). Third-party code: Dear ImGui (MIT), MinHook (BSD-2-Clause), OpenXR SDK headers and loader (Apache-2.0). Their licenses are in `third_party/` and `xrbridge/third_party/`.
