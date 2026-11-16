#!/usr/bin/env bash
# try1.sh <N> <tag> <feats> : compila output_N.cpp de c3 con las transformaciones <feats> (Switch) -> exp/<tag>_N.o y tamano
set -euo pipefail
cd /home/djvemo/nsmbw-port/r9/r33g7
N=$1; T=$2; F=$3; D=exp/src_$T; mkdir -p $D
if [[ $F == none ]]; then cp ../r30g6c3/c3/export/output_$N.cpp $D/; else
python3 - "$N" "$F" "$D" <<'PY'
import sys; sys.path.insert(0, 'tools'); import apply_g7
N, F, D = sys.argv[1:]
t, n = apply_g7.transform(open(f"../r30g6c3/c3/export/output_{N}.cpp").read(), set(F.split(",")), impl=(N == "0" or bool(__import__("os").environ.get("IMPL"))))
open(f'{D}/output_{N}.cpp', 'w').write(t)
PY
fi
cp tools/g7.h $D/; for f in functions.h g6c_decls.h; do ln -sf $PWD/../r30g6c3/c3/export/$f $D/$f; done
~/nsmbw-port/scripts/lim tools/compile1.sh ${W:-nx} $D/output_$N.cpp exp/${T}_$N.o
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-size exp/${T}_$N.o | tail -1
