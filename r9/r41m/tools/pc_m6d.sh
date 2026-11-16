#!/usr/bin/env bash
# [r41m] pc_m6d.sh <n>: m6d en PC SIN interruptores (= valores por defecto de m6_defaults): FC 5 demos con las
# comprobaciones de D/R2 (+ trabajador ×6, render +12 ms), checks 1x por defecto, y una corrida corta con NWII_M6_OFF=1
# y otra con apagados sueltos para ver la línea [M6].
cd /home/djvemo/nsmbw-port/r9/r41m; N=${1:?n}
CK=(NWII_D_DROPFAST_CHECK=1 NWII_R2_PREHASH_CHECK=1 NWII_R2_CLEARTEX_CHECK=1 NWII_GPU_XF_BASEINST=0 NWII_SHADER_SSO=1)
{ echo "env: (por defecto) ${CK[*]} NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000"
  test/demos_fc.sh bin/NSMBW_${N}_input_test ${N}_def 100 "${CK[@]}" NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000
  for d in desierto4j agua_pinguino cielo_paratroopas nubes_plataformas castillo; do L=pc/runs/fc_${N}_def_$d/run.log
    echo "$d: $(grep -a '^\[M6\]' $L | cut -c1-400) | $(grep -a '\[R2PRE\].*check' $L | tail -1 | grep -o 'check: .*') | $(grep -a '\[R2CLR\]' $L | tail -1 | cut -c1-90) | [V] $(grep -a -c '^\[V\]' $L)"; done
  echo FIN; } > test/fc_${N}_def.txt 2>&1
tools/pc_checks_m6.sh $N ${N}_def "${CK[@]}"
for v in "NWII_M6_OFF=1" "NWII_V_LATE_OFF=1 NWII_R2_OFF=1" "NWII_V_LATE=4 NWII_R_OFF=1"; do
  t=m6dline_$(echo $v | tr -c 'A-Za-z0-9\n' _ | cut -c1-40)
  flock pc/.runlock tools/pcrun.sh bin/NSMBW_$N $t 25 $v
  echo "$v -> $(grep -a '^\[M6\]' pc/runs/$t/run.log | cut -c1-400) | [V] $(grep -a -c '^\[V\]' pc/runs/$t/run.log) [R2] $(grep -a -c '^\[R2\]' pc/runs/$t/run.log)"
done >> test/fc_${N}_def.txt
