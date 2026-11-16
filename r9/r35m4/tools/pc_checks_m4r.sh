#!/usr/bin/env bash
# pc_checks_m4r.sh: checks.sh 1x de m4r con S2 y los cinco NWII_R_* encendidos; cmp_png contra m4n encendido -> diag/checks_m4r.txt
V=/home/djvemo/nsmbw-port/r9/r35m4; R=/home/djvemo/nsmbw-port/r9/render; cd $V; O=diag/checks_m4r.txt; : > $O
NWII_S2=1 NWII_S2_CHECK=1 NWII_R_COALESCE=1 NWII_R_PREFIX=1 NWII_R_FREECHUNK=1 NWII_R_CULL=1 NWII_R_TGMEMO=1 RESS=1 NWII_PINDEX_CHECK=1 NWII_DLMEMO_CHECK=1 \
  flock $R/pc/.runlock $R/test/checks.sh $V/bin/NSMBW_m4r_input_test m4r_onR >> $O 2>&1
for s in title map walk lvl11; do L=$R/pc/runs/chk_m4r_onR_${s}_1x
  echo "$s: vs m4n_on $(python3 -W ignore tools/cmp_png.py $R/pc/runs/chk_m4n_on_${s}_1x $L | tail -1) | vs m3n $(python3 -W ignore tools/cmp_png.py $R/pc/runs/chk_t31_m3n_${s}_1x $L | tail -1) | $(grep -a '\[S2\] ACTIVO' $L/run.log | tail -1 | grep -o 'ACTIVO([0-9])\|total:.*' | tr '\n' ' ') $(grep -a '\[S2\]' $L/run.log | grep -a 'AVISO\|ASSERT\|ERROR' | head -2) $(grep -a 'FASTMEM\] dump (exit)' $L/run.log | cut -c1-50) exit=$(tail -1 $L/run.log | cut -c1-30)"; done >> $O
echo FIN >> $O
