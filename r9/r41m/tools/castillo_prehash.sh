#!/usr/bin/env bash
# [r41m] castillo_prehash.sh <bin_input_test> <tag> <n>: FC de castillo (100 s, 1,5789x, FRAMECHECK cada 10, trabajador ×6,
# render +12 ms, valores por defecto + comprobaciones de D/R2) n veces; cuenta PREHASH_CHECK. -> test/castillo_<tag>.txt
cd /home/djvemo/nsmbw-port/r9/r41m; B=$1; T=$2; N=$3
CK=(NWII_D_DROPFAST_CHECK=1 NWII_R2_PREHASH_CHECK=1 NWII_R2_CLEARTEX_CHECK=1 NWII_GPU_XF_BASEINST=0 NWII_SHADER_SSO=1 NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000)
O=test/castillo_$T.txt; : > $O
for i in $(seq 1 $N); do t=cast_${T}_$i
  ( flock pc/.runlock env NWII_FORCE_DEMO=castillo NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 NWII_R9_FRAMECHECK=10 NWII_R_COALESCE_CHECK=1 \
      "${CK[@]}" EVERY=0 PROFILE=free RES=1.5789 pc/run.sh $B $t 100 mp "0-100:100:20:4" > /dev/null 2>&1 )
  L=pc/runs/$t/run.log
  echo "#$i: $(grep -a FRAMECHECK $L | tail -1 | sed 's/.*checks=/checks=/; s/ (pass1.*//') | $(grep -a '\[R2PRE\]' $L | tail -1 | grep -o 'check: .*') | $(tail -1 $L | cut -c1-30)" >> $O
done; echo FIN >> $O
