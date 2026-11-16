#!/usr/bin/env bash
# [r41m] pc_m6f.sh <n>: verificación de m6f en PC con los valores por defecto (sin interruptores):
#  (1) FC 5 demos con TODAS las comprobaciones de D (DROPFAST/LIGHTPARSE/FASTDECODE/FASTPREP_CHECK) y R2, trabajador ×6,
#      render +12 ms; (2) cielo ×6 de 120 s con FRAMECHECK (riesgo abierto de D: render casi parado + SIGSEGV en Mesa);
#  (3) checks 1x; (4) estrés (c) + STRESS_IRQ 5×150 s.
cd /home/djvemo/nsmbw-port/r9/r41m; N=${1:?n}
CK=(NWII_D_DROPFAST_CHECK=1 NWII_D_LIGHTPARSE_CHECK=1 NWII_D_FASTDECODE_CHECK=1 NWII_D_FASTPREP_CHECK=1
    NWII_R2_PREHASH_CHECK=1 NWII_R2_CLEARTEX_CHECK=1 NWII_GPU_XF_BASEINST=0 NWII_SHADER_SSO=1)
SL=(NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000)
dchk() { for k in '\[D40CHECK\]' '\[D40DEC_CHECK\]' '\[D40PREP_CHECK\]' '\[D40L\] parses'; do grep -a "^$k" $1 | tail -1 | grep -oE '(distintos|mismatches)=[0-9]+|prediccion fallida [0-9]+' | tr '\n' ' '; done; }
{ echo "env: (por defecto) ${CK[*]} ${SL[*]}"
  test/demos_fc.sh bin/NSMBW_${N}_input_test ${N}_def 100 "${CK[@]}" "${SL[@]}"
  for d in desierto4j agua_pinguino cielo_paratroopas nubes_plataformas castillo; do L=pc/runs/fc_${N}_def_$d/run.log
    echo "$d: $(grep -a '^\[M6\]' $L | grep -o 'apagados.*' | cut -c1-120) | comprob. D: $(dchk $L) | $(grep -a '\[R2PRE\].*check' $L | tail -1 | grep -o 'check: .*') | VIGIA $(grep -a -c '^\[VIGIA\] #' $L) | $(tail -1 $L | cut -c1-40)"; done
  echo FIN; } > test/fc_${N}_def.txt 2>&1
{ echo "env: (por defecto) ${CK[*]} ${SL[*]} — cielo_paratroopas 120 s ×6"
  for i in 1 2 3 4 5 6; do
    t=cielo_${N}_$i
    ( flock pc/.runlock env NWII_FORCE_DEMO=cielo_paratroopas NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 NWII_R9_FRAMECHECK=10 \
        NWII_R_COALESCE_CHECK=1 "${CK[@]}" "${SL[@]}" EVERY=0 PROFILE=free RES=1.5789 pc/run.sh bin/NSMBW_${N}_input_test $t 120 mp "0-120:100:20:4" > /dev/null 2>&1 )
    L=pc/runs/$t/run.log
    echo "#$i: $(grep -a FRAMECHECK $L | tail -1 | sed 's/.*checks=/checks=/; s/ (pass1.*//') diff_lines=$(grep -a FRAMECHECK $L | grep -vc ' 0 pixels differ') | VIGIA $(grep -a -c '^\[VIGIA\] #' $L) | SEGV $(grep -a -c -i 'segmentation\|SIGSEGV\|core dumped' $L) | comprob. D: $(dchk $L) | $(tail -1 $L | cut -c1-40)"
  done; echo FIN; } > test/cielo_${N}.txt 2>&1
tools/pc_checks_m6.sh $N ${N}_def NWII_D_DROPFAST_CHECK=1 NWII_D_LIGHTPARSE_CHECK=1 NWII_D_FASTDECODE_CHECK=1 NWII_D_FASTPREP_CHECK=1
tools/estres_m6e.sh bin/NSMBW_$N $N 5
