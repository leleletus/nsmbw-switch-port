#!/usr/bin/env bash
# rbuild.sh <c3|...> [nx|pc|both] : compile the 87 output_*.cpp of <c>/export (Switch and/or PC) with the saved
# compile templates (tools/templates/*.cmd = r14g5's commands). Meant for the other laptop:
#   scripts/remoto.sh push r9/r30g6c3/tools r9/r30g6c3/<c>/export
#   scripts/remoto.sh run g6<c> 'JOBS=10 r9/r30g6c3/tools/rbuild.sh <c> both'
#   scripts/remoto.sh pull r9/r30g6c3/<c>/nx_obj r9/r30g6c3/<c>/pc_obj
set -euo pipefail
C=/home/djvemo/nsmbw-port/r9/r30g6c3; E=$1; W=${2:-both}; cd $C
for w in nx pc; do
  [[ $W == both || $W == $w ]] || continue
  [[ -f $E/${w}_obj/.ok ]] && { echo "$w objs already there"; continue; }
  echo "[$(date +%T)] $w objs start"
  TMPL=$C/tools/templates/$w.cmd JOBS=${JOBS:-10} tools/build_g6.sh $w $E/export $E/${w}_obj
  touch $E/${w}_obj/.ok; echo "[$(date +%T)] $w objs ok"
done
