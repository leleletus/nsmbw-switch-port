#!/usr/bin/env bash
# [r41m] estres_m6.sh <bin>: prueba (c): prioridades de Horizon emuladas en PC (NWII_C_PCSCHED=1) + estrés del candado
# del FIFO de GX (entorno de C03), desierto4j<->agua_pinguino. 1) apagado 150 s, 2) V+D encendidos 150 s,
# 3) control con las esperas de m5 (NWII_C_SPIN_OFF=1) 60 s: debe colgarse (y el vigía avisar).
cd /home/djvemo/nsmbw-port/r9/r41m
B=${1:?bin}
E=(NWII_NO_SAVE=1 NWII_FORCE_DEMO=desierto4j,agua_pinguino NWII_FORCE_DEMO_LOOP=1 NWII_C_PCSCHED=1 NWII_C_STRESS_FIFO_US=300 NWII_C_STRESS_SFIFO=1)
ON=(NWII_V_WAKE=1 NWII_V_LATE=8 NWII_D_DROPFAST=1)
flock pc/.runlock tools/pcrun.sh $B estres_off 150 "${E[@]}"
flock pc/.runlock tools/pcrun.sh $B estres_on 150 "${E[@]}" "${ON[@]}"
flock pc/.runlock tools/pcrun.sh $B estres_spinoff_on 60 "${E[@]}" "${ON[@]}" NWII_C_SPIN_OFF=1
for r in estres_off estres_on estres_spinoff_on; do L=pc/runs/$r/run.log
  echo "== $r: FPS $(grep -a '^\[FPS' $L | awk '{s+=$2;n++} END{printf "%.1f media, %d líneas", s/n, n}') | VIGIA $(grep -a -c '^\[VIGIA\] #' $L) | escenas $(grep -ao 'FORCE_DEMO[^ ]* [a-z_0-9]*' $L | sort | uniq -c | tr '\n' ' ') | $(tail -1 $L)"
  grep -a '^\[S2\] ACTIVO' $L | tail -1 | cut -c1-160
  grep -a '^\[VIGIA\]' $L | head -6 | cut -c1-220
done
