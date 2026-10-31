#!/bin/bash
# Builds XrBridge with clang-cl (zig's clang 22) + lld-link against the MSVC/Windows SDK in $TC (default /home/claude/tc).
#   dist/XrBridgeHost.exe        64-bit OpenXR host (ship this next to the game exe with every mod)
#   dist/XrBridgeTest_d3d9.exe   32-bit D3D9Ex test client (end-to-end check without any game)
set -e
cd "$(dirname "$0")"
B=${TC:-/home/claude/tc}/build.sh
mkdir -p build/x86 build/x64 build/x64c dist
CFLAGS="/O2 /Zi /W3 /Iinclude -D_CRT_SECURE_NO_WARNINGS -Wno-unused-function -Wno-missing-field-initializers -Wno-unknown-pragmas -Wno-sign-compare -Wno-unused-variable"
COMMON="src/common/log.cpp src/common/config.cpp"
CLIENT="src/client/client.cpp src/client/ring_d3d9.cpp"

# ---- 64-bit OpenXR host
$B x64 dist/XrBridgeHost.exe $CFLAGS /Fobuild/x64/ /Fdbuild/x64/xrhost.pdb src/host/main.cpp $COMMON \
   /Ithird_party/openxr/include -DXR_USE_PLATFORM_WIN32 -DXR_USE_GRAPHICS_API_D3D11 \
   /link /DEBUG /PDB:build/x64/XrBridgeHost.pdb third_party/openxr/lib/openxr_loader_static.lib \
   user32.lib gdi32.lib d3d11.lib dxgi.lib dxguid.lib advapi32.lib shlwapi.lib pathcch.lib ole32.lib shell32.lib /SUBSYSTEM:WINDOWS

# ---- 32-bit D3D9Ex test client
$B x86 dist/XrBridgeTest_d3d9.exe $CFLAGS /arch:SSE2 /Fobuild/x86/ /Fdbuild/x86/test.pdb test/d3d9test.cpp $CLIENT $COMMON \
   /link /DEBUG /PDB:build/x86/XrBridgeTest_d3d9.pdb user32.lib d3d9.lib advapi32.lib /SUBSYSTEM:WINDOWS

# ---- compile check: client library as 64-bit (for 64-bit games)
$B x64 build/x64c/client_check.exe $CFLAGS /c /Fobuild/x64c/ $CLIENT $COMMON
ls -la dist
