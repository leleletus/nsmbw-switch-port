#!/usr/bin/env bash
# build_tu.sh <src.cpp> <out.o> [extra flags...]: compile one generated TU for the Switch
# exactly like switch/nsmbw/build does (devkitA64, -O2 -ffp-contract=off), plus extra flags.
# Run through scripts/lim (the name build*.sh takes one of the 2 heavy slots).
set -euo pipefail
C=$(cd "$(dirname "$0")/.." && pwd)
src=$1 out=$2; shift 2
EXP=${EXPORT:-$C/export}
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-g++ ${PRE_FLAGS:-} -DNWII_PLATFORM_SWITCH=1 -D_longjmp=longjmp -D_setjmp=setjmp \
  -I"$EXP" -I"$C/nWiiRuntime/src" -I"$C/nWiiRuntime/include" \
  -I$HOME/nsmbw-port/switch/nsmbw/build/_deps/glad-build/include \
  -isystem /opt/devkitpro/portlibs/switch/include -isystem /opt/devkitpro/portlibs/switch/include/SDL2 \
  -isystem $HOME/nsmbw-port/switch/nsmbw/build/_deps/tomlplusplus-src/include -isystem /opt/devkitpro/libnx/include \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -ffunction-sections -fdata-sections \
  -D__SWITCH__ -O2 -DNDEBUG -ffp-contract=off -std=gnu++20 -fPIE -D_REENTRANT "$@" -o "$out" -c "$src"
