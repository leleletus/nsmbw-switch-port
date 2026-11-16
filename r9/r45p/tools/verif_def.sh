#!/usr/bin/env bash
# [r45p] verif_def.sh <bin> <tag>: verificación de PC de un binario con los valores por defecto nuevos (sin variables P):
#  FC 5 demos (1.5789x) + checks 1x + mapa/selector, con NWII_P_DLMERGE_CHECK=10, NWII_D_MERGE_CHECK=10, NWII_D_DROPFAST_CHECK=1;
#  estrés (c) 3x150 s normal y 2x150 s con el render y el trabajador frenados (retiradas y descartes en lote).
B=$(realpath "$1"); T=$2; cd /home/djvemo/nsmbw-port/r9/r45p
tools/pc_verif_mrt.sh "${B}_input_test" $T NWII_P_DLMERGE_CHECK=10 NWII_D_MERGE_CHECK=10 NWII_D_DROPFAST_CHECK=1
tools/estres_p.sh "$B" ${T} ${NEST:-3}
tools/estres_p.sh "$B" ${T}_lento ${NESTL:-2} NWII_R11_SLOW_RENDER_US=15000 NWII_D_SLOWWORKER=3
for f in /home/djvemo/nsmbw-port/r9/r41m/pc/runs/{fc,chk,pant}_${T}_*; do
  [[ -f $f/run.log ]] || continue
  echo "$(basename $f): $(grep -a '^\[D40MERGE_CHECK\]' $f/run.log | tail -1 | cut -c1-70) | distintas $(grep -a '^\[D40MERGE_CHECK\]' $f/run.log | grep -vc ' 0 pixels differ') | $(grep -a '^\[M6\]' $f/run.log | head -1 | grep -o 'NWII_P_[A-Z]*=[0-9]*' | tr '\n' ' ')"
done > test/mergechk_$T.txt
echo HECHO >> test/mergechk_$T.txt
