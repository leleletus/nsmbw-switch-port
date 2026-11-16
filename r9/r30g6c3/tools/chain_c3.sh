#!/usr/bin/env bash
# chain_c3.sh <c3|...> : everything after <c>/{nx_obj,pc_obj} exist (compiled on the other laptop with rbuild.sh
# and pulled), one step at a time on this laptop, each heavy step through ~/nsmbw-port/scripts/lim:
#   static measure vs e2 -> link with m2's runtime (build_c3.sh) -> fastmem table -> comparator (A = e2 PC
#   objects, B = <c>; 2 seeds, --dispatch) -> PC runs (checks sanity title cb4 long lista; each takes .runlock)
# REMOTE_DT=1: skip the comparator here (run tools/rdifftest.sh on the other laptop instead).
# Log: runs/chain_<c>.log
set -u
C=/home/djvemo/nsmbw-port/r9/r30g6c3; L=~/nsmbw-port/scripts/lim; E=$1; N=g6${E}m
cd $C
log() { echo "[$(date +%T)] $*"; }
exec >> runs/chain_$E.log 2>&1
log "chain $E start"
[[ -f $E/nx_obj/.ok && -f $E/pc_obj/.ok ]] || { log "objects missing"; exit 1; }
python3 tools/static_cmp.py e2=../r20g6/e2/nx_obj c1=../r23g6c/c1/nx_obj $E=$E/nx_obj --top 40 > runs/static_$E.txt 2>&1; head -5 runs/static_$E.txt
$L tools/build_c3.sh $E > runs/link_$E.log 2>&1 || { log "link FAILED"; exit 1; }
log "linked: $(tail -5 runs/link_$E.log | tr '\n' ' ')"
python3 ../r14g5/tools/check_fm_tab.py bin/nsmbw_$N.elf ../r14g5/sites.tsv > runs/fmtab_$E.txt 2>&1; log "fastmem: $(tail -2 runs/fmtab_$E.txt | tr '\n' ' ')"
if [[ -n ${REMOTE_DT:-} ]]; then
  tools/verify_c3.sh $N checks sanity title cb4 long lista > runs/verify_$E.log 2>&1; log "pc runs done"; log "chain $E done"; exit 0
fi
mkdir -p difftest/objB_$E
for f in $E/pc_obj/output_*.o; do b=$(basename $f .o); ln -sf $C/$f difftest/objB_$E/$b.cpp.o; done
A=${DT_A:-$C/../r20g6/difftest/objB_e2}
$L bash difftest/build.sh $A difftest/objB_$E difftest/w_$E > difftest/build_$E.log 2>&1 || { log "difftest build FAILED"; exit 1; }
log "difftest built: $(grep '_g6 bodies' difftest/build_$E.log)"
( tools/verify_c3.sh $N checks sanity title cb4 long lista > runs/verify_$E.log 2>&1; log "pc runs done" ) &
vp=$!
mkdir -p difftest/runs; (cd difftest && for s in 1 2; do $L ./run_all.sh w_$E runs/$E $s 2 --dispatch ../gen/jt.txt; done) > runs/difftest_$E.txt 2>&1
log "difftest: $(cat runs/difftest_$E.txt | tr '\n' ' ')"
wait $vp
log "chain $E done"
