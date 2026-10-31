#!/bin/bash
# Recreates the Linux cross-build environment for Windows x86/x64 from toolchain32bit.zip + toolchain64bit.zip.
#   usage: setup_toolchain.sh <dir containing the two zips> [target dir, default /home/claude/tc]
# Produces <target>/build.sh:  build.sh <x86|x64> <output> [clang-cl args...] [/link linker args...]
set -e
ZIPS=$1; TC=${2:-/home/claude/tc}
mkdir -p "$TC"; cd "$TC"
unzip -q -o "$ZIPS/toolchain64bit.zip"
unzip -q -o "$ZIPS/toolchain32bit.zip" 2>/dev/null || true     # this zip uses backslash separators (warning only)

# clang >= 22 is required by this MSVC STL: use zig's clang
pip install --break-system-packages -q ziglang
Z=$(python3 -c "import ziglang,os;print(os.path.dirname(ziglang.__file__))")
command -v lld-link >/dev/null || { echo "need lld-link (apt install lld)"; exit 1; }

# lld-link on Linux is case-sensitive: add lower-case aliases for every library
for a in x86 x64; do
  out=$TC/libs-$a; mkdir -p $out
  if [ $a = x86 ]; then crt=Lib/x86; else crt=lib/x64; fi
  for d in $crt Lib/10.0.28000.0/um/$a Lib/10.0.28000.0/ucrt/$a; do
    for f in $d/*; do [ -f "$f" ] || continue; b=$(basename "$f"); l=$(echo "$b" | tr A-Z a-z)
      ln -sf "$TC/$f" "$out/$l"; [ "$b" != "$l" ] && ln -sf "$TC/$f" "$out/$b"; done
  done
done

# headers: case-insensitive VFS overlay (windows.h vs Windows.h etc.)
python3 - "$TC" <<'PY'
import os, json, sys
tc=sys.argv[1]; roots=[]
for d in ["include","Include/10.0.28000.0/ucrt","Include/10.0.28000.0/um","Include/10.0.28000.0/shared"]:
    for dp,dn,fn in os.walk(os.path.join(tc,d)):
        if fn: roots.append({"name":dp,"type":"directory","contents":[{"name":f,"type":"file","external-contents":os.path.join(dp,f)} for f in fn]})
json.dump({"version":0,"case-sensitive":False,"roots":roots},open(os.path.join(tc,"ci-overlay.yaml"),"w"))
PY

cat > $TC/build.sh <<EOB
#!/bin/bash
set -e
TC=$TC
Z=$Z
ARCH=\$1; OUT=\$2; shift 2
if [ "\$ARCH" = x86 ]; then TGT=i686-pc-windows-msvc; M=X86; else TGT=x86_64-pc-windows-msvc; M=X64; fi
CARGS=(); LARGS=(); inlink=0
for a in "\$@"; do if [ "\$a" = "/link" ]; then inlink=1; continue; fi; if [ \$inlink = 1 ]; then LARGS+=("\$a"); else CARGS+=("\$a"); fi; done
exec \$Z/zig clang --driver-mode=cl -resource-dir \$Z/lib -imsvc \$Z/lib/include --target=\$TGT -fuse-ld=lld-link \\
  /std:c++17 /EHsc /MT /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE /DNOMINMAX -Wno-microsoft-include -Wno-nonportable-include-path -Wno-pragma-pack \\
  /clang:-ivfsoverlay /clang:\$TC/ci-overlay.yaml -imsvc \$TC/include -imsvc \$TC/Include/10.0.28000.0/ucrt \\
  -imsvc \$TC/Include/10.0.28000.0/um -imsvc \$TC/Include/10.0.28000.0/shared \\
  "\${CARGS[@]}" /Fe"\$OUT" /link /LIBPATH:\$TC/libs-\$ARCH /MACHINE:\$M "\${LARGS[@]}"
EOB
chmod +x $TC/build.sh
echo "toolchain ready: $TC/build.sh"
