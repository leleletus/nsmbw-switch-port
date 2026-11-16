#!/usr/bin/env bash
# [r44w] chk_niveles.sh <bin> <tag> <secs> <nivel...>: NWII_FORCE_LEVEL=<nivel> (arranca directo en el nivel, Mario quieto)
# con NWII_W_AGUA_CHECK=1 NWII_S2_OFF=1 NWII_W_AGUA_TODOS=1; una línea por nivel con el resumen y las rutas.
cd /home/djvemo/nsmbw-port/r9/r44w; B=$1; T=$2; S=$3; shift 3
O=test/chk_$T.txt; : > $O
for n in "$@"; do
  flock pc/.runlock tools/pcrun.sh $B chkn_${T}_$n $S NWII_NO_SAVE=1 NWII_FORCE_LEVEL=$n NWII_W_AGUA_CHECK=1 NWII_S2_OFF=1 NWII_W_AGUA_TODOS=1
  L=pc/runs/chkn_${T}_$n/run.log
  echo "$n: $(grep -a 'WAGUA\] check:' $L | tail -1 | cut -c9-) | $(grep -a 'WAGUA\] rutas' $L | tail -1 | cut -c9-) | fps $(grep -a '^\[FPS\]' $L | tail -1 | cut -c7-12) | $(tail -1 $L)" >> $O
  grep -a 'MEMORIA\|REGISTROS\|check: PIPE' $L | head -6 >> $O
done
echo FIN >> $O
