#!/usr/bin/env bash
# Compile one TU of r12fm for the Switch (flags of build_nx.sh, fastmem on). tu_nx.sh <src> <out.o>
D=/opt/devkitpro; C=${TU_C:-/home/djvemo/nsmbw-port/r9/r14g5}
exec nice $D/devkitA64/bin/aarch64-none-elf-g++ -DNWII_PLATFORM_SWITCH=1 -DNWII_FASTMEM=1 -I$C/nWiiRuntime/include -I$C/nWiiRuntime/src \
  -isystem $D/portlibs/switch/include -isystem $D/portlibs/switch/include/SDL2 -isystem $D/libnx/include \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec -ffunction-sections -fdata-sections -D__SWITCH__ \
  -O2 -DNDEBUG -ffp-contract=off -fomit-frame-pointer -fvisibility=hidden -std=gnu++20 -fPIC -D_REENTRANT -I$C/export ${TU_FLAGS:-} -c "$1" -o "$2"
