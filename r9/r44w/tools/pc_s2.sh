#!/usr/bin/env bash
# [r44w] pc_s2.sh <n>: verificación de PC de NWII_W_SIM (bin/NSMBW_<n>): comprobación en agua (real y nula, 300 s),
# FRAMECHECK 5 demos y checks 1x con AGUA+WGP+SIM y las comprobaciones de M.
cd /home/djvemo/nsmbw-port/r9/r44w; N=${1:?n}
CK=(NWII_D_DROPFAST_CHECK=1 NWII_R2_PREHASH_CHECK=1 NWII_R2_CLEARTEX_CHECK=1 NWII_GPU_XF_BASEINST=0 NWII_SHADER_SSO=1)
O=test/chk_sim_$N.txt; : > $O
for m in real null; do
  ex=""; [[ $m == null ]] && ex=NWII_W_AGUA_CHECK_NULL=1
  flock pc/.runlock tools/pcrun.sh bin/NSMBW_$N chksim_${N}_$m 300 NWII_NO_SAVE=1 NWII_FORCE_DEMO=agua_pinguino NWII_FORCE_DEMO_LOOP=1 \
    NWII_W_SIM_CHECK=1 NWII_S2_OFF=1 $ex
  L=pc/runs/chksim_${N}_$m/run.log
  { echo "== $m: $(grep -a 'WSIM\] check:' $L | tail -1)"; grep -a 'MEMORIA\|REGISTROS\|check: PIPE' $L | head -30; tail -1 $L; } >> $O
done
{ echo "env: AGUA+WGP+SIM ${CK[*]} NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000"
  test/demos_fc.sh bin/NSMBW_${N}_input_test ${N}_on 100 NWII_W_AGUA=1 NWII_W_WGP=1 NWII_W_SIM=1 "${CK[@]}" NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000
  for d in agua_pinguino; do L=pc/runs/fc_${N}_on_$d/run.log; echo "$d: $(grep -a 'WSIM\]' $L | tail -1 | cut -c1-120)"; done
  echo FIN; } > test/fc_${N}_on.txt 2>&1
tools/pc_checks_m6.sh $N ${N}_on NWII_W_AGUA=1 NWII_W_WGP=1 NWII_W_SIM=1 "${CK[@]}"
echo HECHO > test/pc_$N.done
