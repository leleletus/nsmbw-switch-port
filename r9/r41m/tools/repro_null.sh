#!/usr/bin/env bash
# [r41m] repro_null.sh <n>: repite la prueba (c) «apagado» (PCSCHED + estrés de C03, desierto4j<->agua) alternando c1 y m6a,
# 150 s cada una; cuenta NullCall (0x8027b930) y avisos del vigía.
cd /home/djvemo/nsmbw-port/r9/r41m
E=(NWII_NO_SAVE=1 NWII_FORCE_DEMO=desierto4j,agua_pinguino NWII_FORCE_DEMO_LOOP=1 NWII_C_PCSCHED=1 NWII_C_STRESS_FIFO_US=300 NWII_C_STRESS_SFIFO=1)
for i in $(seq 1 ${1:-2}); do
  for b in ${BINS:-c1:../r38c/bin/NSMBW_c1 m6a:bin/NSMBW_m6a}; do
    t=${b%%:*}; name=rep${TAGR:-}_${t}_$i
    flock pc/.runlock tools/pcrun.sh ${b#*:} $name ${SECS:-150} "${E[@]}"
    L=pc/runs/$name/run.log
    echo "$name: nullcall $(grep -a -c NullCall $L) vigia $(grep -a -c '^\[VIGIA\] #' $L) fps_ultimo $(grep -a '^\[FPS' $L | tail -1 | cut -c1-30) | $(tail -1 $L)"
  done
done
