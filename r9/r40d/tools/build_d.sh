#!/usr/bin/env bash
# [r40d] build_d.sh (copia de r37v/tools/build_v.sh) <nombre>: PC (NSMBW_<nombre> + _input_test) con el runtime de r9/r37v/nWiiRuntime y los
# objetos de juego m4 de r35m4 (= m5 sin PGO en PC). Copia de r35m4/tools/build_m5.sh (parte pc, RT=g7).
# Correr con ~/nsmbw-port/scripts/lim; JOBS (<=5).
set -euo pipefail
N=${1:?nombre}; E=m4
P=/home/djvemo/nsmbw-port; V=$P/r9/r35m4; W=$P/r9/r40d; M=$P/r9/r24m2; G5=$P/r9/r14g5; C3=$P/r9/r30g6c3
J=${JOBS:-4}; mkdir -p "$W/bin"
PCCMD=$C3/nx/pc_link_g6c3m.cmd
PJ=$W/pcproj B=$W/pc_build DEPS=$P/recomp/export/build/_deps
mkdir -p "$PJ"
cp -f "$G5/export/CMakeLists.txt" "$PJ/"
for f in "$G5"/export/output_*.cpp "$G5/export/main_output.cpp" "$G5/export/fastdispatch.cpp" "$G5/export/functions.h"; do ln -sf "$f" "$PJ/"; done
ln -sfn "$W/nWiiRuntime" "$PJ/nWiiRuntime"
if [[ ! -f $B/build.ninja ]]; then
  cmake -S "$PJ" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold \
    -DFETCHCONTENT_SOURCE_DIR_SDL2=$DEPS/sdl2-src -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$DEPS/tomlplusplus-src \
    -DFETCHCONTENT_SOURCE_DIR_GLAD=$DEPS/glad-src \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DSDL_PIPEWIRE=OFF \
    -DCMAKE_C_FLAGS="-Wno-error=incompatible-pointer-types" > "$W/configure_pc.log"
fi
nice -n 5 ninja -C "$B" -j"$J" nWiiRuntime/libnwiiruntime.a CMakeFiles/NSMBW.dir/nWiiRuntime/src/core/main.cpp.o
MAINO=$B/CMakeFiles/NSMBW.dir/nWiiRuntime/src/core/main.cpp.o; LIB=$B/nWiiRuntime/libnwiiruntime.a
cmd=$(sed "s#/r9/r30g6c3/c3/pc_obj/#/r9/r35m4/$E/pc_obj/#g" $PCCMD)
cmd=${cmd/$M\/rt\/pc\/main.cpp.o/$MAINO}
cmd=${cmd/$M\/rt\/pc\/libnwiiruntime.a/$LIB}
[[ $cmd != *r24m2/rt* && $cmd != *r30g6c3* ]] || { echo "build_d: quedan piezas ajenas en el enlace" >&2; exit 3; }
[[ $(tr ' ' '\n' <<<"$cmd" | grep -c "$V/$E/pc_obj/output_") == 87 ]] || { echo "build_d: faltan objetos" >&2; exit 3; }
(cd $G5/export/build && eval "${cmd/ -o NSMBW / -o $W/bin/NSMBW_$N }")
(cd $G5/export/build && eval "${cmd/ -o NSMBW / -o $W/bin/NSMBW_${N}_input_test $M/rt/pc/input_script.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }")
ls -la "$W/bin/NSMBW_$N" "$W/bin/NSMBW_${N}_input_test"
