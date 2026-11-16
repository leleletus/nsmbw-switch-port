#!/usr/bin/env bash
# pc_checks_c38.sh <nombre>: checks.sh 1x (title/map/walk/lvl11) con S2+R por defecto y NWII_S2_CHECK=1, cmp_png contra m5
# por defecto (r9/render/pc/runs/chk_m5_def_*), contadores de S2 y salida del proceso. -> runs/checks_<nombre>.txt
N=${1:?nombre}; V=/home/djvemo/nsmbw-port/r9/r38c; R=/home/djvemo/nsmbw-port/r9/render; cd $V; O=runs/checks_$N.txt; : > $O
B=$V/bin/NSMBW_${N}_input_test
RESS=1 NWII_S2_CHECK=1 flock $R/pc/.runlock $R/test/checks.sh $B c38_$N >> $O 2>&1
for s in title map walk lvl11; do L=$R/pc/runs/chk_c38_${N}_${s}_1x
  echo "$s: vs m5_def $(python3 -W ignore ../r35m4/tools/cmp_png.py $R/pc/runs/chk_m5_def_${s}_1x $L | tail -1) | S2 $(grep -a '\[S2\] ACTIVO' $L/run.log | tail -1 | grep -o 'ACTIVO([0-9])\|total:.*' | tr '\n' ' ') | vigia $(grep -a -c '\[VIGIA\] #' $L/run.log) | $(tail -1 $L/run.log | cut -c1-30)"; done >> $O
echo FIN >> $O
