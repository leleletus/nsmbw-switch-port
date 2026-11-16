#!/usr/bin/env bash
# build_dx.sh rt|link|gcovlink      (correr con ~/nsmbw-port/scripts/lim; JOBS=2 por defecto)
# r32dx (frente DX): m2 de PC con el runtime de r9/r32dx/nWiiRuntime (= r7f + dx.cpp, contadores por escena).
#   rt        compila solo el runtime (libnwiiruntime.a + main.cpp.o) en r32dx/pc_build
#   (los dos binarios llevan la entrada por guion de PC, como los _input_test: SOLO PC)
#   link      bin/NSMBW_dx        = línea de enlace de m2 (objetos G6 e2 de r20g6/e2/pc_obj) + este runtime
#   gcovlink  bin/NSMBW_dxgcov    = lo mismo con los objetos instrumentados (gcov) de r32dx/pc_gcov + dx_gcov.o
# No escribe nada fuera de r9/r32dx.
set -euo pipefail
WHAT=${1:?rt|link|gcovlink}
P=/home/djvemo/nsmbw-port; V=$P/r9/r32dx; M=$P/r9/r24m2; G5=$P/r9/r14g5; G=$P/r9/r20g6
J=${JOBS:-2}; mkdir -p "$V/bin"
B=$V/pc_build
for h in cpu_context.h config.h nwii_env.h event_scheduler.h hw/hw.h; do
  cmp -s "$V/nWiiRuntime/include/runtime/$h" "$M/nWiiRuntime_r7f_src/include/runtime/$h" ||
    { echo "build_dx: include/runtime/$h difiere del de m2" >&2; exit 2; }
done
linkcmd() {
  local cmd; cmd=$(cat $M/nx/pc_link_m2.cmd)
  cmd=${cmd/$M\/rt\/pc\/main.cpp.o/$B/CMakeFiles/NSMBW.dir/nWiiRuntime/src/core/main.cpp.o}
  cmd=${cmd/$M\/rt\/pc\/libnwiiruntime.a/$B/nWiiRuntime/libnwiiruntime.a}
  [[ $cmd != *r24m2/rt* ]] || { echo "build_dx: queda runtime de m2 en el enlace" >&2; exit 3; }
  echo "$cmd"
}
case $WHAT in
rt)
  PJ=$V/pcproj; DEPS=$P/recomp/export/build/_deps
  mkdir -p "$PJ"
  cp -f "$G5/export/CMakeLists.txt" "$PJ/"
  for f in "$G5"/export/output_*.cpp "$G5/export/main_output.cpp" "$G5/export/fastdispatch.cpp" "$G5/export/functions.h"; do ln -sf "$f" "$PJ/"; done
  ln -sfn "$V/nWiiRuntime" "$PJ/nWiiRuntime"
  if [[ ! -f $B/build.ninja ]]; then
    cmake -S "$PJ" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
      -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold \
      -DFETCHCONTENT_SOURCE_DIR_SDL2=$DEPS/sdl2-src -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$DEPS/tomlplusplus-src \
      -DFETCHCONTENT_SOURCE_DIR_GLAD=$DEPS/glad-src \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DSDL_PIPEWIRE=OFF \
      -DCMAKE_C_FLAGS="-Wno-error=incompatible-pointer-types" > "$V/configure_pc.log"
  fi
  nice -n 5 ninja -C "$B" -j"$J" nWiiRuntime/libnwiiruntime.a CMakeFiles/NSMBW.dir/nWiiRuntime/src/core/main.cpp.o ;;
link)
  cmd=$(linkcmd)
  (cd $G5/export/build && eval "${cmd/ -o NSMBW / -o $V/bin/NSMBW_dx${SUF:-} $M/rt/pc/input_script.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }")
  ls -la "$V/bin/NSMBW_dx${SUF:-}" ;;
gcovlink)
  cmd=$(linkcmd)
  [[ $(ls $V/pc_gcov/output_*.o | wc -l) == 87 ]] || { echo "faltan objetos gcov" >&2; exit 4; }
  cmd=${cmd//$G\/e2\/pc_obj\/output_/$V/pc_gcov/output_}
  [[ $cmd != *e2/pc_obj* ]] || { echo "quedan objetos sin instrumentar" >&2; exit 5; }
  g++ -O2 -c $V/tools/dx_gcov.cpp -o $V/pc_gcov/dx_gcov.o
  (cd $G5/export/build && eval "${cmd/ -o NSMBW / -o $V/bin/NSMBW_dxgcov${SUF:-} $V/pc_gcov/dx_gcov.o --coverage $M/rt/pc/input_script.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }")
  ls -la "$V/bin/NSMBW_dxgcov${SUF:-}" ;;
esac
