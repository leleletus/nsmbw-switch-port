#!/usr/bin/env bash
# Compile one TU (r14g5) for PC with the flags of the full build (quick check). tu_pc.sh <src> <out.o> [extra flags]
C=${TU_C:-/home/djvemo/nsmbw-port/r9/r14g5}
R=/home/djvemo/nsmbw-port/r9/r10c/export/build
src=$1; out=$2; shift 2
exec nice g++ -I$C/export/nWiiRuntime/src -I$C/export/nWiiRuntime/include -I$R/_deps/sdl2-build/include \
  -I$R/_deps/sdl2-build/include/SDL2 -I$R/_deps/sdl2-build/include-config-release/SDL2 -I$R/_deps/glad-build/include \
  -isystem /home/djvemo/nsmbw-port/recomp/export/build/_deps/tomlplusplus-src/include -O3 -DNDEBUG -std=gnu++20 "$@" -c "$src" -o "$out"
