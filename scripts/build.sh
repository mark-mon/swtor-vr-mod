#!/bin/bash
# Cross-builds SWTOR VR on Linux (clang-cl from zig + lld-link, MSVC STL + Windows SDK from toolchain64bit.zip).
#   dist/d3d9.dll           permanent loader (install once next to swtor.exe)
#   dist/swtorvr_core.dll   hot-reloadable mod core (goes to swtorvr\ next to swtor.exe)
set -e
cd "$(dirname "$0")/.."
B=${TC:-/home/claude/tc}/build.sh
mkdir -p build/loader build/core dist
WARN="-Wno-unused-function -Wno-missing-field-initializers -Wno-unknown-pragmas -Wno-sign-compare -Wno-unused-variable \
 -Wno-unused-command-line-argument -Wno-unused-parameter -Wno-microsoft-cast"
CF="/O2 /Zi /W3 -D_CRT_SECURE_NO_WARNINGS -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DUNICODE -D_UNICODE $WARN"
MH="third_party/minhook/src/buffer.c third_party/minhook/src/hook.c third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c"
what=${1:-all}
if [ "$what" = all ] || [ "$what" = loader ]; then
  $B x64 dist/d3d9.dll $CF /LD /Fobuild/loader/ /Fdbuild/loader/vc.pdb \
     src/loader/loader.cpp src/common/log.cpp src/common/config.cpp \
     /link /DEF:src/loader/d3d9.def /DEBUG /PDB:dist/d3d9.pdb user32.lib kernel32.lib
  rm -f dist/d3d9.lib dist/d3d9.exp
fi
if [ "$what" = all ] || [ "$what" = core ]; then
  BUILDID=$(date -u +%Y%m%d_%H%M%S)
  $B x64 dist/swtorvr_core.dll $CF /EHa /LD /Fobuild/core/ /Fdbuild/core/vc.pdb -Ithird_party/minhook/include -Ithird_party/minhook/src -Isrc -Iinclude \
     -DSVR_BUILD_ID=\"$BUILDID\" \
     -Isrc/vrkit -Ithird_party/imgui \
     src/core/*.cpp src/vrkit/*.cpp third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp \
     third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp src/xrbridge/client.cpp src/xrbridge/ring_d3d9.cpp src/common/log.cpp src/common/config.cpp $MH \
     /link /DEBUG /PDB:dist/swtorvr_core.pdb user32.lib gdi32.lib kernel32.lib d3d9.lib psapi.lib
  rm -f dist/swtorvr_core.lib dist/swtorvr_core.exp
fi
ls -la dist
