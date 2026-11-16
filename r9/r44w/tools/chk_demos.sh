#!/usr/bin/env bash
# [r44w] chk_demos.sh <bin> <tag> <secs> [demos...]: NWII_W_AGUA_CHECK=1 (+ NWII_S2_OFF=1) en cada demo; una línea por demo
# con el último resumen [WAGUA] check y las llamadas por tipo; las diferencias (si hay) en test/chk_<tag>.txt.
cd /home/djvemo/nsmbw-port/r9/r44w; B=$1; T=$2; S=$3; shift 3
O=test/chk_$T.txt; : > $O
for d in ${@:-desierto4j agua_pinguino cielo_paratroopas nubes_plataformas castillo}; do
  flock pc/.runlock tools/pcrun.sh $B chk_${T}_$d $S NWII_NO_SAVE=1 NWII_FORCE_DEMO=$d NWII_FORCE_DEMO_LOOP=1 NWII_W_AGUA_CHECK=1 NWII_S2_OFF=1 ${WEXTRA:-}
  L=pc/runs/chk_${T}_$d/run.log
  echo "$d: $(grep -a 'WAGUA\] check:' $L | tail -1 | cut -c9-) | tipos: $(grep -a 'WAGUA\] nativo' $L | grep -o 'tipo [^)]*' | awk '{print}' | sort | uniq -c | sort -rn | head -2 | tr '\n' ';') | fps $(grep -a '^\[FPS\]' $L | tail -1 | cut -c7-12) | $(tail -1 $L)" >> $O
  grep -a 'MEMORIA\|REGISTROS\|check: PIPE' $L | head -12 >> $O
done
echo FIN >> $O
