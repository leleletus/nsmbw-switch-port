#!/usr/bin/env bash
# [r42k] encolar_k.sh <nro> <nombre> [debug extra adicional ';'-separado]: trabajo de 400 s con el entorno fijo del frente K.
set -euo pipefail
N=$(realpath "$1"); NAME=$2; X=${3:-}
E='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo";NWII_FORCE_DEMO_LOOP="1";NWII_V_WAKE="1";NWII_V_LATE="8";NWII_D_DROPFAST="1"'
[[ -n $X ]] && E="$E;$X"
cd /home/djvemo/nsmbw-port/tools/switchctl
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$E" ./cola.sh add "$N" "$NAME" 400
echo "$(date '+%F %T') $NAME <- $N [$X]" >> /home/djvemo/nsmbw-port/r9/r42k/logs/cola_k.txt
