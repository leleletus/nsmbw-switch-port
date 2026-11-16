#!/usr/bin/env bash
# [r44w] build_w.sh <nombre>: PC (NSMBW_<nombre> + _input_test) = build_m6.sh de r41m con el runtime de r9/r44w/nWiiRuntime + w_agua_gen.o (renderizador nativo del agua). Correr con scripts/lim; JOBS<=5.
# objetos de juego m4 de r35m4 (= m5 sin PGO en PC). Copia de r35m4/tools/build_m5.sh (parte pc, RT=g7).
# Correr con ~/nsmbw-port/scripts/lim; JOBS (<=5).
# m6b: SUF=_m6b RTD=nWiiRuntime_m6b tools/build_m6.sh m6b (proyecto y build aparte).
set -euo pipefail
N=${1:?nombre}; E=m4
P=/home/djvemo/nsmbw-port; V=$P/r9/r35m4; W=$P/r9/r44w; M=$P/r9/r24m2; G5=$P/r9/r14g5; C3=$P/r9/r30g6c3
J=${JOBS:-4}; mkdir -p "$W/bin"
PCCMD=$C3/nx/pc_link_g6c3m.cmd
PJ=$W/pcproj${SUF:-} B=$W/pc_build${SUF:-} DEPS=$P/recomp/export/build/_deps
mkdir -p "$PJ"
cp -f "$G5/export/CMakeLists.txt" "$PJ/"
for f in "$G5"/export/output_*.cpp "$G5/export/main_output.cpp" "$G5/export/fastdispatch.cpp" "$G5/export/functions.h"; do ln -sf "$f" "$PJ/"; done
ln -sfn "$W/${RTD:-nWiiRuntime}" "$PJ/nWiiRuntime"
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
[[ $cmd != *r24m2/rt* && $cmd != *r30g6c3* ]] || { echo "build_m6: quedan piezas ajenas en el enlace" >&2; exit 3; }
[[ $(tr ' ' '\n' <<<"$cmd" | grep -c "$V/$E/pc_obj/output_") == 87 ]] || { echo "build_m6: faltan objetos" >&2; exit 3; }
# r44w: el renderizador nativo del agua, compilado con la MISMA orden que los objetos del juego en PC (pc_obj de m4)
WO=$W/w_agua/pc_obj; mkdir -p $WO
python3 -I $W/tools/gen_w_agua.py $V/export $W/w_agua/w_agua_gen.cpp
[[ -n ${WSIM_KEEP:-} ]] || $W/tools/gen_w_sim_all.sh $V/export $W/w_agua/w_sim_gen.inc
tmpl=$(cat $V/$E/pc_obj/compile.cmd); tmpl=${tmpl//\$out/$WO/w_agua_gen.o}; tmpl=${tmpl//\$in/$W/w_agua/w_agua_gen.cpp}
(cd $G5/export/build && nice -n 5 bash -c "$tmpl -I$W/w_agua")
cmd=${cmd/ -o NSMBW / $WO/w_agua_gen.o -o NSMBW }
(cd $G5/export/build && eval "${cmd/ -o NSMBW / -o $W/bin/NSMBW_$N }")
(cd $G5/export/build && eval "${cmd/ -o NSMBW / -o $W/bin/NSMBW_${N}_input_test $M/rt/pc/input_script.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }")
ls -la "$W/bin/NSMBW_$N" "$W/bin/NSMBW_${N}_input_test"
