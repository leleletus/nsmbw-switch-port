#!/usr/bin/env bash
# [r37v] (otra laptop) v37_pgo_remote.sh <nombre>: m5 PGO + runtime de r37v. Como m4r/m5 (r35m4 §6/§11): el runtime
# NO lleva perfil (PGO_RUNTIME=OFF), asi que se recompilan solo las fuentes cambiadas (gx, ios, os_hle_stubs, main)
# con las ordenes EXACTAS de nx/m5_pgo y se enlaza con los MISMOS objetos PGO de m4/m5. No toca m5src ni nx/m5_pgo.
# Fuentes: ~/nsmbw-port/r9/r37v/remote_src/<ruta> (rsync desde la laptop principal).
set -euo pipefail
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro} DEVKITA64=${DEVKITA64:-/opt/devkitpro/devkitA64}
N=${1:?nombre}
C=$HOME/nsmbw-port/r9/r17pgo; cd $C
S=$C/${N}src; B=$C/nx/${N}_pgo; R=$HOME/nsmbw-port/r9/r37v/remote_src
FILES="src/hle/gx.cpp src/hle/ios.cpp src/core/os_hle_stubs.cpp src/core/main.cpp"
rm -rf "$S" "$B"; mkdir -p "$S"
cp -a --reflink=auto m5src/nWiiRuntime "$S/nWiiRuntime"
for f in $FILES; do cp "$R/$f" "$S/nWiiRuntime/$f"; done
cp -a --reflink=auto nx/m5_pgo "$B"
cd nx/m5_pgo
O=runtime/nwiiruntime/CMakeFiles/nwiiruntime.dir/src
MAINO=$(ninja -t targets all | grep -o '^CMakeFiles/nsmbw.dir/[^:]*src/core/main\.o' | head -1)
{
  for t in $O/hle/gx.o $O/hle/ios.o $O/core/os_hle_stubs.o $MAINO runtime/nwiiruntime/libnwiiruntime.a nsmbw.elf nsmbw.nro; do
    ninja -t commands -s "$t"
  done
} > "$B/v37_cmds.raw"
cd "$B"
sed -e "s#r17pgo/m5src/nWiiRuntime/src/hle/gx\.cpp#r17pgo/${N}src/nWiiRuntime/src/hle/gx.cpp#g" \
    -e "s#r17pgo/m[45]src/nWiiRuntime/src/hle/ios\.cpp#r17pgo/${N}src/nWiiRuntime/src/hle/ios.cpp#g" \
    -e "s#r17pgo/m[45]src/nWiiRuntime/src/core/os_hle_stubs\.cpp#r17pgo/${N}src/nWiiRuntime/src/core/os_hle_stubs.cpp#g" \
    -e "s#r17pgo/m[45]src/nWiiRuntime/src/core/main\.cpp#r17pgo/${N}src/nWiiRuntime/src/core/main.cpp#g" \
    -e "s#r17pgo/nx/m5_pgo#r17pgo/nx/${N}_pgo#g" v37_cmds.raw > v37_cmds.sh
[[ $(grep -c "${N}src" v37_cmds.sh) == 4 ]] || { echo "v37: esperaba 4 fuentes cambiadas"; grep -c "${N}src" v37_cmds.sh; exit 3; }
grep -q "m5_pgo" v37_cmds.sh && { echo "v37: quedan rutas de m5_pgo"; exit 3; }
rm -f nsmbw.elf nsmbw.nro $O/hle/gx.o $O/hle/ios.o $O/core/os_hle_stubs.o "$MAINO"
bash -e v37_cmds.sh
mkdir -p $C/bin
cp nsmbw.nro $C/bin/${N}_pgo.nro; cp nsmbw.elf $C/bin/${N}_pgo.elf
python3 $C/tools/check_fm_tab_g5.py $C/bin/${N}_pgo.elf 2>&1 | tail -2
sha256sum $C/bin/${N}_pgo.nro $C/bin/${N}_pgo.elf
