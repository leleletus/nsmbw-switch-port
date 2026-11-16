#!/usr/bin/env bash
# rdifftest.sh <c3|c3b> [parts] : comparator by function, A = e2 PC objects (r20g6/difftest/objB_e2), B = <c>/pc_obj,
# all 40 031 functions, seeds 1 and 2, --dispatch gen/jt.txt. For the other laptop (12 threads): parts default 10.
#   -> runs/difftest_<c>.txt, difftest/runs/<c>_s*_*.txt
set -u
C=/home/djvemo/nsmbw-port/r9/r30g6c3; E=$1; P=${2:-10}; cd $C
mkdir -p difftest/objB_$E difftest/runs runs
for f in $E/pc_obj/output_*.o; do b=$(basename $f .o); ln -sf $C/$f difftest/objB_$E/$b.cpp.o; done
bash difftest/build.sh $C/../r20g6/difftest/objB_e2 difftest/objB_$E difftest/w_$E > difftest/build_$E.log 2>&1 || { echo "difftest build FAILED"; exit 1; }
grep '_g6 bodies' difftest/build_$E.log
(cd difftest && for s in 1 2; do ./run_all.sh w_$E runs/$E $s $P --dispatch ../gen/jt.txt; done) | tee runs/difftest_$E.txt
