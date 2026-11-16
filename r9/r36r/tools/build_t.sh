#!/usr/bin/env bash
# build_t.sh <nombre> [pc|nx|both]     (correr con ~/nsmbw-port/scripts/lim; JOBS=2 por defecto)
# r31t (frente T): m2 con el runtime de r9/r31t/nWiiRuntime (copia editada del de r7f).
# Solo compila el runtime (libnwiiruntime.a + main.o) en r31t/{nx/build,pc_build} y enlaza con las
# MISMAS lineas de m2 (r24m2/nx/link_m2.cmd, pc_link_m2.cmd: codigo generado G6 e2, orden L g6e2)
# cambiando solo las piezas del runtime. No escribe nada fuera de r9/r36r.
#   -> bin/nsmbw_<nombre>.nro/.elf (+ _input_test.nro, SOLO pruebas)   bin/NSMBW_<nombre> (+ _input_test)
set -euo pipefail
N=${1:?nombre}; WHAT=${2:-both}
P=/home/djvemo/nsmbw-port; V=$P/r9/r36r; M=$P/r9/r24m2; G5=$P/r9/r14g5
# CODE=c3: codigo generado G6c3 (r9/r30g6c3/c3/{nx_obj,pc_obj}, su linea de enlace y su orden L) en vez de G6 e2.
C3=$P/r9/r30g6c3
if [[ ${CODE:-e2} == c3 ]]; then
  NXCMD=$C3/nx/link_g6c3m.cmd; PCCMD=$C3/nx/pc_link_g6c3m.cmd; ORDER=$C3/nx/order/g6c3m.txt
else
  NXCMD=$M/nx/link_m2.cmd; PCCMD=$M/nx/pc_link_m2.cmd; ORDER=$M/nx/order/g6e2.txt
fi
export PATH="$HOME/.local/bin:$PATH" DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
DKP=$DEVKITPRO; J=${JOBS:-2}; mkdir -p "$V/bin"
for h in cpu_context.h config.h nwii_env.h event_scheduler.h hw/hw.h; do
  cmp -s "$V/nWiiRuntime/include/runtime/$h" "$M/nWiiRuntime_r7f_src/include/runtime/$h" ||
    { echo "build_t: include/runtime/$h difiere del de m2: el codigo generado no casaria. Me niego." >&2; exit 2; }
done

pc() {
  local PJ=$V/pcproj B=$V/pc_build DEPS=$P/recomp/export/build/_deps
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
  nice -n 5 ninja -C "$B" -j"$J" nWiiRuntime/libnwiiruntime.a CMakeFiles/NSMBW.dir/nWiiRuntime/src/core/main.cpp.o
  local cmd; cmd=$(cat $PCCMD)
  cmd=${cmd/$M\/rt\/pc\/main.cpp.o/$B/CMakeFiles/NSMBW.dir/nWiiRuntime/src/core/main.cpp.o}
  cmd=${cmd/$M\/rt\/pc\/libnwiiruntime.a/$B/nWiiRuntime/libnwiiruntime.a}
  [[ $cmd != *r24m2/rt* ]] || { echo "build_t: queda runtime de m2 en el enlace de PC" >&2; exit 3; }
  (cd $G5/export/build && eval "${cmd/ -o NSMBW / -o $V/bin/NSMBW_$N }")
  (cd $G5/export/build && eval "${cmd/ -o NSMBW / -o $V/bin/NSMBW_${N}_input_test $M/rt/pc/input_script.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }")
  ls -la "$V/bin/NSMBW_$N"
}

nx() {
  local B=$V/nx/build D=$V/nx/$N
  mkdir -p "$B" "$D/game" "$D/test"
  if [[ ! -f $B/build.ninja ]]; then
    cmake -S $P/switch/nsmbw -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$G5/cmake/nsmbw-switch.cmake \
      -DCMAKE_BUILD_TYPE=Release -DNWII_RUNTIME_DIR="$V/nWiiRuntime" -DNSMBW_EXPORT_DIR="$G5/export" \
      -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$P/switch/nsmbw/build/_deps/tomlplusplus-src \
      -DFETCHCONTENT_SOURCE_DIR_GLAD=$P/switch/nsmbw/build/_deps/glad-src \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache > "$V/configure_nx.log"
  fi
  local MAINO
  MAINO=$(cd "$B" && ninja -t targets all | grep -o '^CMakeFiles/nsmbw.dir/[^:]*src/core/main.o' | head -1)
  nice -n 5 ninja -C "$B" -j"$J" runtime/nwiiruntime/libnwiiruntime.a "$MAINO"
  local cmd; cmd=$(cat $NXCMD)
  cmd=${cmd/$M\/rt\/nx\/main.o/$B/$MAINO}
  cmd=${cmd/$M\/rt\/nx\/libnwiiruntime.a/$B/runtime/nwiiruntime/libnwiiruntime.a}
  [[ $cmd == *"$B/$MAINO"* && $cmd == *"$B/runtime/nwiiruntime/libnwiiruntime.a"* ]] || { echo "build_t: piezas no cambiadas en el enlace" >&2; exit 3; }
  local O=$ORDER
  (cd $G5/nx/build && eval "${cmd/-o nsmbw.elf/-o $D/game/nsmbw.elf -Wl,--section-ordering-file=$O}")
  $DKP/tools/bin/nacptool --create "NSMBW PRUEBA $N (${CODE:-e2} + runtime R)" "nsmbw-port" "$N" "$D/game/nsmbw.nacp" > /dev/null
  $DKP/tools/bin/elf2nro "$D/game/nsmbw.elf" "$V/bin/nsmbw_$N.nro" --icon=$DKP/libnx/default_icon.jpg --nacp="$D/game/nsmbw.nacp" > /dev/null
  cp -f "$D/game/nsmbw.elf" "$V/bin/nsmbw_$N.elf"
  local tcmd=${cmd/-o nsmbw.elf/-o $D/test/nsmbw.elf -Wl,--section-ordering-file=$O}
  tcmd=${tcmd/ -fPIE / -fPIE $M/rt/nx/input_script_nx.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }
  (cd $G5/nx/build && eval "$tcmd")
  $DKP/tools/bin/elf2nro "$D/test/nsmbw.elf" "$V/bin/nsmbw_${N}_input_test.nro" --icon=$DKP/libnx/default_icon.jpg --nacp="$D/game/nsmbw.nacp" > /dev/null
  rm -f "$D/test/nsmbw.elf"
  ls -la "$V/bin/nsmbw_$N.nro" "$V/bin/nsmbw_$N.elf"
}
case $WHAT in pc) pc ;; nx) nx ;; both) pc; nx ;; esac
