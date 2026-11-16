#!/usr/bin/env bash
# tools/test_nro.sh <variant>: from a finished build_pgo.sh build (nx/<variant>), link the
# citron-only test NRO bin/<variant>_input_test.nro = the same link + tools/input_script.cpp
# (scripted input from debug.toml NWII_INPUT_SCRIPT, [SCENE] log) with --wrap of KPADRead,
# exactly as build_nx.sh does. ONLY for citron (r9/base/citron_scene.sh). Never for the console
# as the game.
# TEST_LINK_EXTRA="..." extra link flags (e.g. the L order file), OUT_SUFFIX=_ord -> bin/<variant>_ord_input_test.nro
set -euo pipefail
C=$(cd "$(dirname "$0")/.." && pwd)
V=${1:?usage: test_nro.sh <variant>}
B=$C/nx/$V
DKP=${DEVKITPRO:-/opt/devkitpro}
T=$(grep -o 'NWII_RUNTIME_DIR:PATH=.*' "$B/CMakeCache.txt" | cut -d= -f2)
GLADINC=$B/_deps/glad-build/include
INPUTO=$B/input_script_nx.o
$DKP/devkitA64/bin/aarch64-none-elf-g++ -DNWII_PLATFORM_SWITCH=1 -DNWII_FASTMEM=1 \
  -I"$T/include" -I"$T/src" -I"$GLADINC" \
  -isystem $DKP/portlibs/switch/include -isystem $DKP/portlibs/switch/include/SDL2 \
  -isystem $DKP/libnx/include \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec \
  -ffunction-sections -fdata-sections -D__SWITCH__ -O2 -DNDEBUG -ffp-contract=off -fomit-frame-pointer -fvisibility=hidden \
  -std=gnu++20 -fPIC -D_REENTRANT -c "$C/tools/input_script.cpp" -o "$INPUTO"
cmd=$(cd "$B" && ninja -t commands nsmbw.elf | tail -1); cmd=${cmd#: && }; cmd=${cmd% && :}
dir=$B/test_input; mkdir -p "$dir"
cmd=${cmd/-o nsmbw.elf/-o $dir/nsmbw.elf}
cmd=${cmd/ -fPIE / -fPIE $INPUTO -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx ${TEST_LINK_EXTRA:-} }
while pgrep -x 'ld|ld.bfd|collect2' > /dev/null; do sleep 10; done
(cd "$B" && eval "nice -n 5 $cmd")
$DKP/tools/bin/nacptool --create "NSMBW Recomp" nsmbw-port 0.2 "$dir/nsmbw.nacp"
$DKP/tools/bin/elf2nro "$dir/nsmbw.elf" "$C/bin/${V}${OUT_SUFFIX:-}_input_test.nro" --icon=$DKP/libnx/default_icon.jpg --nacp="$dir/nsmbw.nacp"
ls -la "$C/bin/${V}${OUT_SUFFIX:-}_input_test.nro"
