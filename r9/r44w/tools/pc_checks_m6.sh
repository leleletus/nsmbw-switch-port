#!/usr/bin/env bash
# [r41m] pc_checks_m6.sh <nombre> <tag> [env...]: test/checks.sh 1x (title/map/walk/lvl11) con NWII_S2_CHECK=1 y el
# entorno dado; cmp_png contra c1 (r9/render/pc/runs/chk_c38_c1_*) y m5 (chk_m5_def_*), contadores de S2, vigía,
# D40CHECK y salida del proceso. -> test/checks_<tag>.txt  (copia adaptada de r38c/tools/pc_checks_c38.sh)
N=${1:?nombre}; tag=${2:?tag}; shift 2
V=/home/djvemo/nsmbw-port/r9/r44w; R=/home/djvemo/nsmbw-port/r9/render/pc/runs; cd $V; O=test/checks_$tag.txt; : > $O
echo "env: $*" >> $O
env RESS=1 NWII_S2_CHECK=1 "$@" flock pc/.runlock test/checks.sh $V/bin/NSMBW_${N}_input_test $tag >> $O 2>&1
for s in title map walk lvl11; do L=$V/pc/runs/chk_${tag}_${s}_1x
  echo "$s: vs c1 $(python3 -W ignore ../r35m4/tools/cmp_png.py $R/chk_c38_c1_${s}_1x $L | tail -1) | vs m5 $(python3 -W ignore ../r35m4/tools/cmp_png.py $R/chk_m5_def_${s}_1x $L | tail -1) | S2 $(grep -a '\[S2\] ACTIVO' $L/run.log | tail -1 | grep -o 'ACTIVO([0-9])\|total:.*' | tr '\n' ' ') | vigia $(grep -a -c '\[VIGIA\] #' $L/run.log) | [V] $(grep -a -c '^\[V\]' $L/run.log) | $(tail -1 $L/run.log | cut -c1-30)"; done >> $O
echo FIN >> $O
