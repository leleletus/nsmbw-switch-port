#!/usr/bin/env bash
# [r41m] pc_m6b.sh: verificación de PC de m6b: (a) checks 1x todo apagado; (b) FC 5 demos con V+D+R2 «todo11» y las
# comprobaciones de D y R2 (modo uniforme + SSO como en la Switch); (b2) checks 1x con todo encendido.
cd /home/djvemo/nsmbw-port/r9/r41m
R2=(NWII_R2_LATEFREE=1 NWII_R2_CLEARTEX=1 NWII_R2_COPYFBO=1 NWII_R2_LAZYNAT=1 NWII_R2_COPYUNI=1 NWII_R2_COPYST=1 NWII_R2_LAZYEFB=1 NWII_R2_PREHASH=1 NWII_R2_TEXMEMO=1 NWII_R_CULL=2 NWII_R2_XDUBO=1)
CK=(NWII_R2_PREHASH_CHECK=1 NWII_R2_CLEARTEX_CHECK=1 NWII_GPU_XF_BASEINST=0 NWII_SHADER_SSO=1)
VD=(NWII_V_WAKE=1 NWII_V_LATE=8 NWII_D_DROPFAST=1 NWII_D_DROPFAST_CHECK=1)
tools/pc_checks_m6.sh m6b m6b_off
{ echo "env: ${VD[*]} ${R2[*]} ${CK[*]} NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000"
  test/demos_fc.sh bin/NSMBW_m6b_input_test m6b_on 100 "${VD[@]}" "${R2[@]}" "${CK[@]}" NWII_D_SLOWWORKER=6 NWII_R11_SLOW_RENDER_US=12000
  for d in desierto4j agua_pinguino cielo_paratroopas nubes_plataformas castillo; do L=pc/runs/fc_m6b_on_$d/run.log
    echo "$d: $(grep -a -i 'prehash.*check\|PREHASH_CHECK\|\[R2PH' $L | tail -1 | cut -c1-160) | $(grep -a -i 'cleartex.*check\|\[R2CT' $L | grep -v 'R2CT\] C' | tail -1 | cut -c1-160) | XDUBO $(grep -a -c 'XDUBO\|R2XDraw' $L)"; done
  echo FIN; } > test/fc_m6b_on.txt 2>&1
tools/pc_checks_m6.sh m6b m6b_on "${VD[@]}" "${R2[@]}" "${CK[@]}"
