#!/usr/bin/env bash
# m4r_pgo_remote.sh (otra laptop): m4r = m4 PGO + parche de render R (r36r_vs_r31t.patch: gx.cpp, renderer_gl.cpp).
# El runtime NO lleva perfil (PGO_RUNTIME=OFF en build_pgo.sh): basta recompilar esos dos archivos y enlazar con los
# MISMOS objetos optimizados de m4. No toca r17pgo/m4src ni nx/m4_pgo_m4_all (copias: m4rsrc, nx/m4r_pgo).
set -euo pipefail
C=$HOME/nsmbw-port/r9/r17pgo; cd $C
[[ -d m4rsrc ]] || { mkdir m4rsrc; cp -a --reflink=auto m4src/nWiiRuntime m4rsrc/nWiiRuntime; }
cd m4rsrc/nWiiRuntime
if ! grep -q NWII_R_COALESCE src/hle/gx.cpp; then patch -p2 < $HOME/nsmbw-port/r9/r36r/r36r_vs_r31t.patch; fi
cd $C
rm -rf nx/m4r_pgo; cp -a --reflink=auto nx/m4_pgo_m4_all nx/m4r_pgo
B=nx/m4r_pgo
# solo las dos fuentes del render pasan a la copia parcheada; la carpeta de compilacion vieja -> la nueva
sed -i -e "s#m4src/nWiiRuntime/src/hle/gx.cpp#m4rsrc/nWiiRuntime/src/hle/gx.cpp#g" \
       -e "s#m4src/nWiiRuntime/src/hle/gx/renderer_gl.cpp#m4rsrc/nWiiRuntime/src/hle/gx/renderer_gl.cpp#g" \
       -e "s#nx/m4_pgo_m4_all#nx/m4r_pgo#g" $B/build.ninja $B/CMakeFiles/rules.ninja 2>/dev/null || true
grep -c m4rsrc $B/build.ninja
ninja -C $B -n -d explain 2>&1 | grep -c 'is dirty' || true
ninja -C $B -n | grep -E 'Building|Linking' | sed 's/.*\(Building\|Linking\)/\1/' | cut -c1-140 | head -20
ninja -C $B -j2
cp $B/nsmbw.nro bin/m4r_pgo.nro; cp $B/nsmbw.elf bin/m4r_pgo.elf
python3 tools/check_fm_tab_g5.py bin/m4r_pgo.elf 2>&1 | tail -2
ls -la bin/m4r_pgo.*
