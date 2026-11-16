#!/usr/bin/env bash
# [r44w] pc_w2.sh <n>: verificación de PC de la variante <n> (bin/NSMBW_<n>): comprobación del agua (real y control
# nulo, 400 s cada una), FRAMECHECK 5 demos con AGUA+WGP y las comprobaciones de M, checks 1x con AGUA+WGP.
cd /home/djvemo/nsmbw-port/r9/r44w; N=${1:?n}
CK=(NWII_D_DROPFAST_CHECK=1 NWII_R2_PREHASH_CHECK=1 NWII_R2_CLEARTEX_CHECK=1 NWII_GPU_XF_BASEINST=0 NWII_SHADER_SSO=1)
O=test/chk_agua_$N.txt; : > $O
for m in real null; do
  ex=""; [[ $m == null ]] && ex=NWII_W_AGUA_CHECK_NULL=1
  flock pc/.runlock tools/pcrun.sh bin/NSMBW_$N chkagua_${N}_$m 400 NWII_NO_SAVE=1 NWII_FORCE_DEMO=agua_pinguino NWII_FORCE_DEMO_LOOP=1 \
    NWII_W_AGUA_CHECK=1 NWII_S2_OFF=1 NWII_W_WGP=1 $ex
  L=pc/runs/chkagua_${N}_$m/run.log
  { echo "== $m: $(grep -a 'WAGUA\] check:' $L | tail -1)"; grep -a 'MEMORIA\|REGISTROS\|check: PIPE\|selftest' $L | head -20; tail -1 $L; } >> $O
done
{ echo "env: AGUA+WGP ${CK[*]} NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000"
  test/demos_fc.sh bin/NSMBW_${N}_input_test ${N}_on 100 NWII_W_AGUA=1 NWII_W_WGP=1 "${CK[@]}" NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000
  for d in desierto4j agua_pinguino cielo_paratroopas nubes_plataformas castillo; do L=pc/runs/fc_${N}_on_$d/run.log
    echo "$d: $(grep -a '^\[M6\]' $L | cut -c1-200) | $(grep -a 'WAGUA\] nativo' $L | tail -1 | cut -c1-120) | $(grep -a 'WGP\] r44w' $L | head -1)"; done
  echo FIN; } > test/fc_${N}_on.txt 2>&1
tools/pc_checks_m6.sh $N ${N}_on NWII_W_AGUA=1 NWII_W_WGP=1 "${CK[@]}"
echo HECHO > test/pc_$N.done
