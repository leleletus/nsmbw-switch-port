#!/usr/bin/env bash
# pc_checks.sh <nombre_bin> : checks.sh 1x apagado y encendido (S2) + cmp_png contra m3n -> diag/checks_<n>.txt
V=/home/djvemo/nsmbw-port/r9/r35m4; N=$1; R=/home/djvemo/nsmbw-port/r9/render; cd $V
B=$V/bin/NSMBW_${N}_input_test; O=diag/checks_$N${SKIP_OFF:+_on}.txt; : > $O
[[ -n ${SKIP_OFF:-} ]] || RESS=1 NWII_PINDEX_CHECK=1 NWII_DLMEMO_CHECK=1 flock $R/pc/.runlock $R/test/checks.sh $B ${N}_off >> $O 2>&1
NWII_S2=1 NWII_S2_CHECK=1 RESS=1 NWII_PINDEX_CHECK=1 NWII_DLMEMO_CHECK=1 flock $R/pc/.runlock $R/test/checks.sh $B ${N}_on >> $O 2>&1
for t in off on; do for s in title map walk lvl11; do L=$R/pc/runs/chk_${N}_${t}_${s}_1x
  echo "$t $s: $(python3 -W ignore tools/cmp_png.py $R/pc/runs/chk_t31_m3n_${s}_1x $L | tail -1) | S2: $(grep -a '\[S2\] ACTIVO' $L/run.log | tail -1 | grep -o 'ACTIVO([0-9])\|total:.*' | tr '\n' ' ') $(grep -a '\[S2\]' $L/run.log | grep -a 'AVISO\|ASSERT\|ERROR' | head -2) forced=$(grep -a -c force-expired $L/run.log) fm=$(grep -a 'FASTMEM\] dump (exit)' $L/run.log | cut -c1-60)"; done; done >> $O
echo FIN >> $O
