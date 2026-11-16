#!/usr/bin/env bash
# [r41m] pc_m6i.sh <n>: verificación de PC de m6i (bin/NSMBW_<n>) con los VALORES POR DEFECTO (MERGE, AGUA, WGP, A_MIX=2,
# NOSPIN, REQSPIN=200 encendidos; LOGBG no existe en PC):
#  P1 FC 5 demos con las comprobaciones de D (+MERGE_CHECK) y R2 + NWII_AX_VERIFY; checks 1x (+AX_VERIFY); estrés (c) 5×150 s.
#  P2 reverb de A: NWII_AXFX_KERNEL=4 NWII_AXFX_KCHECK=1 (kernel 4 contra el 1 en cada llamada), 2 demos 150 s.
#  P3 NWII_W_AGUA_CHECK (+S2_OFF, como W) en un sitio de cada ruta: agua_pinguino (B530), 8-C (BA70 lava), 2-C (C5C0),
#     5-4 (CA50 pantano), 2-5 (C020 arena movediza); 150 s cada uno.
cd /home/djvemo/nsmbw-port/r9/r41m; N=${1:?n}
CK=(NWII_D_DROPFAST_CHECK=1 NWII_D_LIGHTPARSE_CHECK=1 NWII_D_FASTDECODE_CHECK=1 NWII_D_FASTPREP_CHECK=1 NWII_R2_CLEARTEX_CHECK=1 NWII_GPU_XF_BASEINST=0 NWII_SHADER_SSO=1)
SL=(NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000)
dchk() { for k in '\[D40CHECK\]' '\[D40DEC_CHECK\]' '\[D40PREP_CHECK\]' '\[D40L\] parses'; do grep -a "^$k" $1 | tail -1 | grep -oE '(distintos|mismatches)=[0-9]+|prediccion fallida [0-9]+' | tr '\n' ' '; done; }
t=${N}_def
{ echo "env: (por defecto) NWII_D_MERGE_CHECK=10 NWII_AX_VERIFY=1 ${CK[*]} ${SL[*]}"
  test/demos_fc.sh bin/NSMBW_${N}_input_test $t 100 NWII_D_MERGE_CHECK=10 NWII_AX_VERIFY=1 "${CK[@]}" "${SL[@]}"
  for d in desierto4j agua_pinguino cielo_paratroopas nubes_plataformas castillo; do L=pc/runs/fc_${t}_$d/run.log
    echo "$d: D: $(dchk $L)| $(grep -a '^\[D40MERGE_CHECK\]' $L | tail -1 | cut -c1-60) | AXV $(grep -a '^\[AXV\]' $L | grep -v MISMATCH | tail -1 | cut -c7-60) MISMATCH $(grep -a -c 'AXV\] MISMATCH' $L) | $(grep -a -m1 '^\[WAGUA\]' $L | cut -c1-60) | VIGIA $(grep -a -c '^\[VIGIA\] #' $L)"; done
  grep -a -m1 '^\[M6\]' pc/runs/fc_${t}_agua_pinguino/run.log
  echo FIN; } > test/fc_$t.txt 2>&1
tools/pc_checks_m6.sh $N $t NWII_AX_VERIFY=1 NWII_D_DROPFAST_CHECK=1
tools/estres_m6e.sh bin/NSMBW_$N $t 5
O=test/kernel4_$N.txt; : > $O
for d in castillo agua_pinguino; do
  flock pc/.runlock tools/pcrun.sh bin/NSMBW_$N k4_${N}_$d 150 NWII_NO_SAVE=1 NWII_FORCE_DEMO=$d NWII_FORCE_DEMO_LOOP=1 NWII_AXFX_KERNEL=4 NWII_AXFX_KCHECK=1
  L=pc/runs/k4_${N}_$d/run.log
  echo "$d: $(grep -a '^\[A2\] axfx' $L | tail -1 | cut -c1-200) | $(grep -a 'A43\] axfx' $L | tail -1 | cut -c1-120) | $(tail -1 $L)" >> $O
done; echo FIN >> $O
O=test/agua_niveles_$N.txt; : > $O
for s in demo:agua_pinguino 8-C 2-C 5-4 2-5; do
  if [[ $s == demo:* ]]; then d=${s#demo:}; ex=(NWII_FORCE_DEMO=$d NWII_FORCE_DEMO_LOOP=1); nm=$d; else ex=(NWII_FORCE_LEVEL=$s); nm=$s; fi
  flock pc/.runlock tools/pcrun.sh bin/NSMBW_$N chkw_${N}_$nm 150 NWII_NO_SAVE=1 "${ex[@]}" NWII_W_AGUA_CHECK=1 NWII_S2_OFF=1
  L=pc/runs/chkw_${N}_$nm/run.log
  echo "$nm: $(grep -a 'WAGUA\] check:' $L | tail -1 | cut -c9-) | $(grep -a 'WAGUA\] rutas' $L | tail -1 | cut -c9-) | $(tail -1 $L)" >> $O
  grep -a 'REGISTROS\|check: PIPE' $L | head -4 >> $O
done; echo FIN >> $O
echo HECHO > test/pc_$N.done
