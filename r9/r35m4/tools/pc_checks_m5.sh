#!/usr/bin/env bash
# pc_checks_m5.sh: m5 sin variables (S2 + R por defecto) y con el apagado (NWII_S2_OFF=1 NWII_R_OFF=1): checks.sh 1x,
# cmp_png contra m4r encendido / m4n apagado, salida del proceso; luego titulo 100 s con audio (apagado) para comparar con la consola.
V=/home/djvemo/nsmbw-port/r9/r35m4; R=/home/djvemo/nsmbw-port/r9/render; cd $V; O=diag/checks_m5.txt; : > $O
B=$V/bin/NSMBW_m5_input_test
RESS=1 NWII_S2_CHECK=1 flock $R/pc/.runlock $R/test/checks.sh $B m5_def >> $O 2>&1
NWII_S2_OFF=1 NWII_R_OFF=1 RESS=1 flock $R/pc/.runlock $R/test/checks.sh $B m5_off >> $O 2>&1
for t in def off; do ref=m4r_onR; [[ $t == off ]] && ref=m4n_off; for s in title map walk lvl11; do L=$R/pc/runs/chk_m5_${t}_${s}_1x
  echo "$t $s: vs $ref $(python3 -W ignore tools/cmp_png.py $R/pc/runs/chk_${ref}_${s}_1x $L | tail -1) | S2 $(grep -a '\[S2\] ACTIVO' $L/run.log | tail -1 | grep -o 'ACTIVO([0-9])\|total:.*' | tr '\n' ' ') $(grep -a -c '\[S2\] sonido en otro nucleo ACTIVADO' $L/run.log) | rcoal=$(grep -a -c '\[RCOAL\]' $L/run.log) | $(tail -1 $L/run.log | cut -c1-30)"; done; done >> $O
echo FIN >> $O
cd /home/djvemo/nsmbw-port/r9/r34s2
flock ../render/pc/.runlock ./tools/audio_run.sh ../r35m4/bin/NSMBW_m5 m5_tit100_off 100 NWII_REALTIME=1 NWII_S2_OFF=1 NWII_R_OFF=1 > /dev/null 2>&1
flock ../render/pc/.runlock ./tools/audio_run.sh ../r35m4/bin/NSMBW_m5 m5_tit100_def 100 NWII_REALTIME=1 > /dev/null 2>&1
echo FIN_AUDIO >> $V/$O
