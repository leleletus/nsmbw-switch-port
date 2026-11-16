#!/usr/bin/env bash
# [r44w] pc_s3.sh <n>: FRAMECHECK 5 demos con AGUA+WGP+SIM=2 y las comprobaciones de M; luego espera a que la máquina
# esté libre (load < 6) y hace los checks 1x (220/220).
cd /home/djvemo/nsmbw-port/r9/r44w; N=${1:?n}
CK=(NWII_D_DROPFAST_CHECK=1 NWII_R2_PREHASH_CHECK=1 NWII_R2_CLEARTEX_CHECK=1 NWII_GPU_XF_BASEINST=0 NWII_SHADER_SSO=1)
{ echo "env: AGUA+WGP+SIM=2 ${CK[*]} NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000 | load $(cut -d' ' -f1 /proc/loadavg)"
  test/demos_fc.sh bin/NSMBW_${N}_input_test ${N}_on 100 NWII_W_AGUA=1 NWII_W_WGP=1 NWII_W_SIM=2 "${CK[@]}" NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000
  L=pc/runs/fc_${N}_on_agua_pinguino/run.log; echo "agua: $(grep -a 'WSIM\]' $L | head -1 | cut -c1-140)"
  echo FIN; } > test/fc_${N}_on.txt 2>&1
for i in $(seq 1 240); do l=$(cut -d' ' -f1 /proc/loadavg); awk -v l=$l 'BEGIN{exit !(l<6)}' && break; sleep 60; done
echo "checks con load $(cut -d' ' -f1 /proc/loadavg)" > test/checks_${N}_on.load
tools/pc_checks_m6.sh $N ${N}_on NWII_W_AGUA=1 NWII_W_WGP=1 NWII_W_SIM=2 "${CK[@]}"
echo HECHO > test/pc_$N.done
