#!/usr/bin/env bash
# pc_verify.sh <etiqueta_bin> : cadena de verificacion de PC del frente S2 (todo bajo render/pc/.runlock, una tras otra)
#   1) checks.sh 1x con NWII_S2=1 NWII_S2_CHECK=1          -> runs/checks_<t>_on_1x.txt
#   2) lista de demos 330 s con S2 y sin S2 + volcado WAV   -> runs/<t>_lista_on, runs/<t>_lista_off, runs/wav_<t>_lista.txt
#   3) lo mismo 200 s con CPU "tipo Switch" (PROFILE=sw)    -> runs/<t>_sw_on, runs/<t>_sw_off, runs/wav_<t>_sw.txt
#   4) ThreadSanitizer (bin/NSMBW_<t>_tsan), titulo 150 s   -> runs/<t>_tsan
V=/home/djvemo/nsmbw-port/r9/r34s2; T=$1; cd $V
LK=../render/pc/.runlock
D='NWII_FORCE_DEMO=desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 NWII_REALTIME=1'
resumen() { grep -a "\[S2\] ACTIVO\|\[S2\] en espera" runs/$1/run.log | tail -1 | grep -o "total:.*"; grep -a "\[S2\]" runs/$1/run.log | grep -a "AVISO\|ASSERT\|ERROR" | head -5; grep -ac "\[DEMO\] start" runs/$1/run.log; }
if [[ -z ${SKIP_CHECKS:-} ]]; then
NWII_S2=1 NWII_S2_CHECK=1 RESS=1 flock $LK ../render/test/checks.sh $V/bin/NSMBW_${T}_input_test ${T}_on > runs/checks_${T}_on_1x.txt 2>&1
for s in title map walk lvl11; do L=../render/pc/runs/chk_${T}_on_${s}_1x/run.log; echo "$s: $(grep -a '\[S2\] ACTIVO' $L | tail -1 | grep -o 'total:.*') $(grep -a '\[S2\]' $L | grep -a 'AVISO\|ASSERT\|ERROR' | head -3)"; done >> runs/checks_${T}_on_1x.txt
fi
flock $LK ./tools/audio_run.sh bin/NSMBW_$T ${T}_lista_on 330 NWII_S2=1 NWII_S2_CHECK=1 $D > /dev/null 2>&1
flock $LK ./tools/audio_run.sh bin/NSMBW_$T ${T}_lista_off 330 $D > /dev/null 2>&1
{ resumen ${T}_lista_on; python3 tools/wavq.py runs/${T}_lista_off/audio.wav runs/${T}_lista_on/audio.wav; } > runs/wav_${T}_lista.txt 2>&1
PROFILE=sw flock $LK ./tools/audio_run.sh bin/NSMBW_$T ${T}_sw_on 200 NWII_S2=1 NWII_S2_CHECK=1 $D > /dev/null 2>&1
PROFILE=sw flock $LK ./tools/audio_run.sh bin/NSMBW_$T ${T}_sw_off 200 $D > /dev/null 2>&1
{ resumen ${T}_sw_on; python3 tools/wavq.py runs/${T}_sw_off/audio.wav runs/${T}_sw_on/audio.wav; } > runs/wav_${T}_sw.txt 2>&1
if [[ -x bin/NSMBW_${T}_tsan ]]; then
  TSAN_OPTIONS="halt_on_error=0 history_size=4 report_signal_unsafe=0 log_path=$V/runs/tsan_$T" flock $LK ./tools/audio_run.sh bin/NSMBW_${T}_tsan ${T}_tsan 150 NWII_S2=1 NWII_S2_CHECK=1 NWII_REALTIME=1 > /dev/null 2>&1
fi
echo "FIN $(date +%T)" > runs/pc_verify_$T.done
