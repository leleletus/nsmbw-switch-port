#!/usr/bin/env bash
# [r38c] copia de r35m4/tools/build_m5.sh: runtime r38c/nWiiRuntime + objetos de m4 (r35m4/m4/pc_obj). Solo PC.
# build_c38.sh <nombre> [pc]   (con ~/nsmbw-port/scripts/lim; JOBS<=5)  -> bin/NSMBW_<nombre> (+ _input_test)
#   RT=t   (por defecto) runtime de r31t ya compilado (r31t/nx/build, r31t/pc_build): solo enlaza. A/B exacto con m3n.
#   RTONLY=1 (con RT=g7): solo compila el runtime.
#   RT=g7  runtime de r9/r33g7/nWiiRuntime (r31t + avisos del sondeo compacto): lo compila en r33g7/{nx/build,pc_build}.
# Codigo generado: r33g7/<variante>/{nx_obj,pc_obj} (87 output_N.o). Linea de enlace y orden L = los de g6c3m
# (r30g6c3/nx/link_g6c3m.cmd, pc_link_g6c3m.cmd, order/g6c3m.txt) cambiando solo los 87 objetos y las piezas del runtime.
#   -> bin/nsmbw_<nombre>.nro/.elf (+ _input_test.nro, SOLO pruebas)   bin/NSMBW_<nombre> (+ _input_test)
set -euo pipefail
N=${1:?nombre}; E=m4; WHAT=${2:-pc}; RT=g7
P=/home/djvemo/nsmbw-port; V=$P/r9/r38c; O=$P/r9/r35m4; M=$P/r9/r24m2; G5=$P/r9/r14g5; C3=$P/r9/r30g6c3; T=$P/r9/r31t
export PATH="$HOME/.local/bin:$PATH" DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
DKP=$DEVKITPRO; J=${JOBS:-2}; mkdir -p "$V/bin" "$V/nx" "$V/runs"
NXCMD=$C3/nx/link_g6c3m.cmd; PCCMD=$C3/nx/pc_link_g6c3m.cmd; ORDER=$C3/nx/order/g6c3m.txt

if [[ $RT == g7 ]]; then
  # el codigo generado se compila con las cabeceras de r14g5: en el runtime de r33g7 solo pueden diferir cuerpos de
  # funciones en linea que el codigo generado no usa (queue_callback, trigger_pi_interrupt). Se comprueba.
  d=$(diff "$G5/nWiiRuntime/include/runtime/cpu_context.h" "$V/nWiiRuntime/include/runtime/cpu_context.h" | grep '^[<>]' || true)
  [[ $(grep -c . <<<"$d") -le 1 && ( -z $d || $d == *"poll_kick();  // [r33-g7]"* ) ]] ||
    { echo "build_g7: cpu_context.h difiere en algo mas que queue_callback: el codigo generado no casaria" >&2; exit 2; }
  for h in config.h nwii_env.h event_scheduler.h; do
    cmp -s "$V/nWiiRuntime/include/runtime/$h" "$G5/nWiiRuntime/include/runtime/$h" || cmp -s "$V/nWiiRuntime/include/runtime/$h" "$T/nWiiRuntime/include/runtime/$h" ||
      { echo "build_g7: include/runtime/$h difiere" >&2; exit 2; }
  done
fi

pc() {
  local MAINO LIB
  if [[ $RT == g7 ]]; then
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
    MAINO=$B/CMakeFiles/NSMBW.dir/nWiiRuntime/src/core/main.cpp.o; LIB=$B/nWiiRuntime/libnwiiruntime.a
    [[ -n ${RTONLY:-} ]] && return 0
  else
    MAINO=$T/pc_build/CMakeFiles/NSMBW.dir/nWiiRuntime/src/core/main.cpp.o; LIB=$T/pc_build/nWiiRuntime/libnwiiruntime.a
  fi
  [[ -f $MAINO && -f $LIB ]] || { echo "build_g7: falta el runtime de PC ($MAINO)" >&2; exit 3; }
  local cmd; cmd=$(sed "s#/r9/r30g6c3/c3/pc_obj/#/r9/r35m4/m4/pc_obj/#g" $PCCMD)
  cmd=${cmd/$M\/rt\/pc\/main.cpp.o/$MAINO}
  cmd=${cmd/$M\/rt\/pc\/libnwiiruntime.a/$LIB}
  [[ $cmd != *r24m2/rt* && $cmd != *r30g6c3* ]] || { echo "build_g7: quedan piezas ajenas en el enlace de PC" >&2; exit 3; }
  [[ $(tr ' ' '\n' <<<"$cmd" | grep -c "$O/m4/pc_obj/output_") == 87 ]] || { echo "build_g7: faltan objetos de PC" >&2; exit 3; }
  (cd $G5/export/build && eval "${cmd/ -o NSMBW / -o $V/bin/NSMBW_$N }")
  (cd $G5/export/build && eval "${cmd/ -o NSMBW / -o $V/bin/NSMBW_${N}_input_test $M/rt/pc/input_script.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }")
  ls -la "$V/bin/NSMBW_$N"
}

nx() {
  local D=$V/nx/$N MAINO LIB
  mkdir -p "$D/game" "$D/test"
  if [[ $RT == g7 ]]; then
    local B=$V/nx/build_m5
    mkdir -p "$B"
    if [[ ! -f $B/build.ninja ]]; then
      cmake -S $P/switch/nsmbw -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$G5/cmake/nsmbw-switch.cmake \
        -DCMAKE_BUILD_TYPE=Release -DNWII_RUNTIME_DIR="$V/nWiiRuntime" -DNSMBW_EXPORT_DIR="$G5/export" \
        -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$P/switch/nsmbw/build/_deps/tomlplusplus-src \
        -DFETCHCONTENT_SOURCE_DIR_GLAD=$P/switch/nsmbw/build/_deps/glad-src \
        -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache > "$V/configure_nx.log"
    fi
    local mo; mo=$(cd "$B" && ninja -t targets all | grep -o '^CMakeFiles/nsmbw.dir/[^:]*src/core/main.o' | head -1)
    nice -n 5 ninja -C "$B" -j"$J" runtime/nwiiruntime/libnwiiruntime.a "$mo"
    MAINO=$B/$mo; LIB=$B/runtime/nwiiruntime/libnwiiruntime.a
    [[ -n ${RTONLY:-} ]] && return 0
  else
    local B=$T/nx/build
    MAINO=$B/$(cd "$B" && ninja -t targets all | grep -o '^CMakeFiles/nsmbw.dir/[^:]*src/core/main.o' | head -1)
    LIB=$B/runtime/nwiiruntime/libnwiiruntime.a
  fi
  [[ -f $MAINO && -f $LIB ]] || { echo "build_g7: falta el runtime de Switch ($MAINO)" >&2; exit 3; }
  local cmd; cmd=$(sed "s#/r9/r30g6c3/c3/nx_obj/#/r9/r35m4/$E/nx_obj/#g" $NXCMD)
  cmd=${cmd/$M\/rt\/nx\/main.o/$MAINO}
  cmd=${cmd/$M\/rt\/nx\/libnwiiruntime.a/$LIB}
  [[ $cmd == *"$MAINO"* && $cmd == *"$LIB"* && $cmd != *r30g6c3* ]] || { echo "build_g7: piezas no cambiadas en el enlace" >&2; exit 3; }
  [[ $(tr ' ' '\n' <<<"$cmd" | grep -c "$V/$E/nx_obj/output_") == 87 ]] || { echo "build_g7: faltan objetos de Switch" >&2; exit 3; }
  echo "$cmd" > "$V/nx/link_$N.cmd"
  (cd $G5/nx/build && eval "${cmd/-o nsmbw.elf/-o $D/game/nsmbw.elf -Wl,--section-ordering-file=$ORDER -Wl,-Map=$D/game/nsmbw.map}")
  $DKP/tools/bin/nacptool --create "NSMBW PRUEBA $N (G7 $E, runtime $RT)" "nsmbw-port" "$N" "$D/game/nsmbw.nacp" > /dev/null
  $DKP/tools/bin/elf2nro "$D/game/nsmbw.elf" "$V/bin/nsmbw_$N.nro" --icon=$DKP/libnx/default_icon.jpg --nacp="$D/game/nsmbw.nacp" > /dev/null
  cp -f "$D/game/nsmbw.elf" "$V/bin/nsmbw_$N.elf"
  local tcmd=${cmd/-o nsmbw.elf/-o $D/test/nsmbw.elf -Wl,--section-ordering-file=$ORDER}
  tcmd=${tcmd/ -fPIE / -fPIE $M/rt/nx/input_script_nx.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }
  (cd $G5/nx/build && eval "$tcmd")
  $DKP/tools/bin/elf2nro "$D/test/nsmbw.elf" "$V/bin/nsmbw_${N}_input_test.nro" --icon=$DKP/libnx/default_icon.jpg --nacp="$D/game/nsmbw.nacp" > /dev/null
  rm -f "$D/test/nsmbw.elf"
  ls -la "$V/bin/nsmbw_$N.nro" "$V/bin/nsmbw_$N.elf"
}
case $WHAT in pc) pc ;; nx) nx ;; both) pc; nx ;; esac
[[ -n ${RTONLY:-} ]] && exit 0
(cd $V/bin && sha256sum $(ls nsmbw_$N.nro nsmbw_$N.elf nsmbw_${N}_input_test.nro NSMBW_$N NSMBW_${N}_input_test 2>/dev/null) > $V/runs/bin_sha256_$N.txt)
