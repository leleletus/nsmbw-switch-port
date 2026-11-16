#!/usr/bin/env bash
# compile1.sh <nx|pc> <file.cpp> <out.o> : one output_*.cpp with the build_g6.sh command (for quick checks)
set -euo pipefail
W=$1; F=$(realpath "$2"); O=$(realpath -m "$3"); E=$(dirname "$F")
T=$(mktemp -d); trap 'rm -rf $T' EXIT
DRY=1 /home/djvemo/nsmbw-port/r9/r33g7/tools/build_g6.sh $W "$E" $T > /dev/null
cmd=$(cat $T/compile.cmd); cmd=${cmd/ccache /}; cmd=${cmd/\$out/$O}; cmd=${cmd/\$in/$F}
if [[ $W == nx ]]; then cd /home/djvemo/nsmbw-port/r9/r14g5/nx/build; else cd /home/djvemo/nsmbw-port/r9/r14g5/export/build; fi
eval "$cmd"
