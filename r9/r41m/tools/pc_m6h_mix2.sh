#!/usr/bin/env bash
# [r41m] pc_m6h_mix2.sh: m6h con la configuración de M29 (MERGE+AGUA+WGP+A_MIX=2+NOSPIN+REQSPIN=200; LOGBG no existe
# en PC) + NWII_AX_VERIFY + MERGE_CHECK: FC 5 demos, checks 1x, estrés (c) 5×150 s.
cd /home/djvemo/nsmbw-port/r9/r41m; N=m6h
CK=(NWII_D_DROPFAST_CHECK=1 NWII_D_LIGHTPARSE_CHECK=1 NWII_D_FASTDECODE_CHECK=1 NWII_D_FASTPREP_CHECK=1 NWII_R2_CLEARTEX_CHECK=1 NWII_GPU_XF_BASEINST=0 NWII_SHADER_SSO=1)
SL=(NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000)
M29=(NWII_D_MERGE=1 NWII_W_AGUA=1 NWII_W_WGP=1 NWII_A_MIX=2 NWII_A_NOSPIN=1 NWII_A_REQSPIN=200)
t=m6h_mix2
{ echo "env: ${M29[*]} NWII_AX_VERIFY=1 NWII_D_MERGE_CHECK=10 ${CK[*]} ${SL[*]}"
  test/demos_fc.sh bin/NSMBW_${N}_input_test $t 100 "${M29[@]}" NWII_AX_VERIFY=1 NWII_D_MERGE_CHECK=10 "${CK[@]}" "${SL[@]}"
  for d in desierto4j agua_pinguino cielo_paratroopas nubes_plataformas castillo; do L=pc/runs/fc_${t}_$d/run.log
    echo "$d: MERGE $(grep -a '^\[D40MERGE_CHECK\]' $L | tail -1 | cut -c1-60) | AXV $(grep -a '^\[AXV\]' $L | grep -v MISMATCH | tail -1 | cut -c1-70) MISMATCH $(grep -a -c 'AXV\] MISMATCH' $L) | $(grep -a '^\[A43\]' $L | tail -1 | grep -o 'nospin.*') | VIGIA $(grep -a -c '^\[VIGIA\] #' $L)"; done
  echo FIN; } > test/fc_$t.txt 2>&1
tools/pc_checks_m6.sh $N $t "${M29[@]}" NWII_AX_VERIFY=1 NWII_D_DROPFAST_CHECK=1
tools/estres_m6e.sh bin/NSMBW_$N $t 5 "${M29[@]}"
