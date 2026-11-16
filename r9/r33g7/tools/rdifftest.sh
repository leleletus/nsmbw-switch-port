#!/usr/bin/env bash
# rdifftest.sh <variante> [procesos=10] : comparador por funcion, A = objetos de PC de c3 (r30g6c3/c3/pc_obj),
# B = r33g7/<variante>/pc_obj; las 40 031 funciones, semillas 1 y 2, --dispatch gen/jt.txt.
# Pensado para la otra laptop (12 hilos); aqui: dentro de scripts/lim y con 2-4 procesos.
#   -> runs/difftest_<variante>.txt, difftest/runs/<variante>_s*_*.txt
set -u
C=/home/djvemo/nsmbw-port/r9/r33g7; E=$1; P=${2:-10}; cd $C
mkdir -p difftest/objA_c3 difftest/objB_$E difftest/runs runs
for f in $C/../r30g6c3/c3/pc_obj/output_*.o; do b=$(basename $f .o); ln -sf $f difftest/objA_c3/$b.cpp.o; done
for f in $E/pc_obj/output_*.o; do b=$(basename $f .o); ln -sf $C/$f difftest/objB_$E/$b.cpp.o; done
bash difftest/build.sh $C/difftest/objA_c3 $C/difftest/objB_$E difftest/w_$E > difftest/build_$E.log 2>&1 || { echo "difftest build FAILED"; tail -5 difftest/build_$E.log; exit 1; }
grep '_g6 bodies' difftest/build_$E.log
(cd difftest && for s in 1 2; do ./run_all.sh w_$E runs/$E $s $P --dispatch ../gen/jt.txt; done) | tee runs/difftest_$E.txt
