#!/usr/bin/env bash
# rbuild_s2.sh [nx|pc|both] : compila los output_N.cpp parcheados de r34s2/s2/export (pasada tools/apply_s2.py)
# con las plantillas de compilacion de G6c3 -> r34s2/s2/{nx_obj,pc_obj}. Pensado para la otra laptop:
#   scripts/remoto.sh push r9/r34s2/tools r9/r34s2/s2/export
#   scripts/remoto.sh run s2_objs 'JOBS=8 r9/r34s2/tools/rbuild_s2.sh both'
#   scripts/remoto.sh pull r9/r34s2/s2/nx_obj r9/r34s2/s2/pc_obj
set -euo pipefail
P=/home/djvemo/nsmbw-port; C=$P/r9/r30g6c3; V=$P/r9/r34s2; W=${1:-both}
for w in nx pc; do
  [[ $W == both || $W == $w ]] || continue
  echo "[$(date +%T)] $w objs start"
  TMPL=$C/tools/templates/$w.cmd JOBS=${JOBS:-8} $C/tools/build_g6.sh $w $V/s2/export $V/s2/${w}_obj
  echo "[$(date +%T)] $w objs ok"
done
