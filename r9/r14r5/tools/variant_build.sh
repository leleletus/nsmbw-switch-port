#!/usr/bin/env bash
# variant_build.sh <variant_dir> <name> [pc|nx|both]   (run it under ~/nsmbw-port/scripts/lim)
#
# Builds a RUNTIME-ONLY variant against the generated code already compiled in BASE (r9/r10c):
#   <variant_dir>/nWiiRuntime/   the variant's runtime (a copy of BASE's, edited)
#   -> <variant_dir>/bin/NSMBW_<name>, NSMBW_<name>_input_test      (PC)
#   -> <variant_dir>/bin/nsmbw_<name>.nro, .elf, _input_test.nro    (Switch)
# Only the runtime library and main.o are compiled (in <variant_dir>/pc_build and nx/build);
# the game code objects are BASE's, so the link line is BASE's with those two swapped.
# Several variants can build at the same time (nothing of BASE is written).
#
# RULE: do NOT change a runtime header the generated code includes (include/runtime/cpu_context.h,
# config.h, nwii_env.h, event_scheduler.h, hw/hw.h): BASE's objects were compiled against BASE's
# copies, and a mismatch breaks the binary in odd ways. Check: diff -q against BASE before building.
set -euo pipefail
V=$(realpath "$1"); N=$2; WHAT=${3:-both}
BASE=${BASE:-/home/djvemo/nsmbw-port/r9/r10c}
P=/home/djvemo/nsmbw-port
export PATH="$HOME/.local/bin:$PATH" DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
J=${JOBS:-3}
mkdir -p "$V/bin"

for h in cpu_context.h config.h nwii_env.h event_scheduler.h hw/hw.h; do
  if ! cmp -s "$V/nWiiRuntime/include/runtime/$h" "$BASE/nWiiRuntime/include/runtime/$h"; then
    echo "variant_build: include/runtime/$h differs from BASE: the generated code would not match. Refusing." >&2
    exit 2
  fi
done

pc() {
  local PJ=$V/pcproj B=$V/pc_build RB=$BASE/export/build DEPS=$P/recomp/export/build/_deps
  mkdir -p "$PJ"
  cp -f "$BASE/export/CMakeLists.txt" "$PJ/"
  for f in "$BASE"/export/output_*.cpp "$BASE/export/main_output.cpp" "$BASE/export/fastdispatch.cpp" "$BASE/export/functions.h"; do
    ln -sf "$f" "$PJ/"
  done
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
  local cmd
  cmd=$(cd "$RB" && ninja -t commands NSMBW | tail -1 | sed "s/^: && //; s/ && :$//")
  cmd=${cmd/-Wl,--dependency-file=CMakeFiles\/NSMBW.dir\/link.d/}
  cmd=${cmd/CMakeFiles\/NSMBW.dir\/nWiiRuntime\/src\/core\/main.cpp.o/$B/CMakeFiles/NSMBW.dir/nWiiRuntime/src/core/main.cpp.o}
  cmd=${cmd/nWiiRuntime\/libnwiiruntime.a/$B/nWiiRuntime/libnwiiruntime.a}
  (cd "$RB" && eval "${cmd/ -o NSMBW / -o $V/bin/NSMBW_$N }")
  local D=$B/_deps INPUTO=$B/input_script.o
  ccache g++ -O2 -std=gnu++20 -I"$V/nWiiRuntime/include" -I"$V/nWiiRuntime/src" \
    -I$DEPS/sdl2-src/include -I"$D/sdl2-build/include" -I"$D/sdl2-build/include-config-release" \
    -c "$BASE/tools/input_script.cpp" -o "$INPUTO"
  (cd "$RB" && eval "${cmd/ -o NSMBW / -o $V/bin/NSMBW_${N}_input_test $INPUTO -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }")
  ls -la "$V/bin/NSMBW_$N" "$V/bin/NSMBW_${N}_input_test"
}

nx() {
  local B=$V/nx/build AB=$BASE/nx/build
  mkdir -p "$B"
  if [[ ! -f $B/build.ninja ]]; then
    cmake -S $P/switch/nsmbw -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$BASE/cmake/nsmbw-switch.cmake \
      -DCMAKE_BUILD_TYPE=Release -DNWII_RUNTIME_DIR="$V/nWiiRuntime" -DNSMBW_EXPORT_DIR="$BASE/export" \
      -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$P/switch/nsmbw/build/_deps/tomlplusplus-src \
      -DFETCHCONTENT_SOURCE_DIR_GLAD=$P/switch/nsmbw/build/_deps/glad-src \
      -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache > "$V/configure_nx.log"
  fi
  local GLAD MAINO
  GLAD=$(cd "$B" && ninja -t targets all | grep -o '^[^:]*libglad\.a' | head -1)
  MAINO=$(cd "$B" && ninja -t targets all | grep -o '^CMakeFiles/nsmbw.dir/[^:]*src/core/main.o' | head -1)
  nice -n 5 ninja -C "$B" -j"$J" runtime/nwiiruntime/libnwiiruntime.a "$GLAD" "$MAINO"
  local cmd
  cmd=$(cd "$AB" && ninja -t commands nsmbw.elf | tail -1); cmd=${cmd#: && }; cmd=${cmd% && :}
  local BREL=${BASE#/}
  cmd=${cmd/CMakeFiles\/nsmbw.dir\/$BREL\/nWiiRuntime\/src\/core\/main.o/$B/$MAINO}
  [[ $cmd == *"$B/$MAINO"* ]] || { echo "variant_build: main.o not swapped in the Switch link line" >&2; exit 3; }
  cmd=${cmd/runtime\/nwiiruntime\/libnwiiruntime.a/$B/runtime/nwiiruntime/libnwiiruntime.a}
  cmd=${cmd/_deps\/glad-build\/libglad.a/$B/$GLAD}
  mkdir -p "$B/link" "$B/link_test"
  (cd "$AB" && eval "${cmd/-o nsmbw.elf/-o $B/link/nsmbw.elf}")
  $DEVKITPRO/tools/bin/nacptool --create "NSMBW Recomp" nsmbw-port 0.2 "$B/link/nsmbw.nacp" > /dev/null
  $DEVKITPRO/tools/bin/elf2nro "$B/link/nsmbw.elf" "$V/bin/nsmbw_$N.nro" --icon=$DEVKITPRO/libnx/default_icon.jpg --nacp="$B/link/nsmbw.nacp"
  cp -f "$B/link/nsmbw.elf" "$V/bin/nsmbw_$N.elf"
  # test-only scripted input (citron / never the real console's shipped build)
  local GLADINC INPUTO=$B/input_script_nx.o
  GLADINC=$(dirname "$B/$GLAD")/include
  ccache $DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++ -DNWII_PLATFORM_SWITCH=1 \
    -I"$V/nWiiRuntime/include" -I"$V/nWiiRuntime/src" -I"$GLADINC" \
    -isystem $DEVKITPRO/portlibs/switch/include -isystem $DEVKITPRO/portlibs/switch/include/SDL2 \
    -isystem $DEVKITPRO/libnx/include \
    -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec \
    -ffunction-sections -fdata-sections -D__SWITCH__ -O2 -DNDEBUG -DNWII_FASTMEM=1 -ffp-contract=off -fomit-frame-pointer -fvisibility=hidden \
    -std=gnu++20 -fPIC -D_REENTRANT -c "$BASE/tools/input_script.cpp" -o "$INPUTO"
  local tcmd=${cmd/-o nsmbw.elf/-o $B/link_test/nsmbw.elf}
  tcmd=${tcmd/ -fPIE / -fPIE $INPUTO -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }
  (cd "$AB" && eval "$tcmd")
  $DEVKITPRO/tools/bin/elf2nro "$B/link_test/nsmbw.elf" "$V/bin/nsmbw_${N}_input_test.nro" --icon=$DEVKITPRO/libnx/default_icon.jpg --nacp="$B/link/nsmbw.nacp"
  ls -la "$V/bin/nsmbw_$N.nro" "$V/bin/nsmbw_${N}_input_test.nro"
}

case $WHAT in
  pc) pc ;;
  nx) nx ;;
  both) pc; nx ;;
  *) echo "usage: $0 <variant_dir> <name> [pc|nx|both]" >&2; exit 1 ;;
esac
