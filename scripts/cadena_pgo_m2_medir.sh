#!/usr/bin/env bash
# cadena_pgo_m2_medir.sh: no-AI. Waits for the remote job pgo_m2_fin -> brings bin/m2_pgo_m2_all*.nro back ->
#   queues on the console P01 (m2+PGO) and P02 (m2 control) with the fixed list (like K02), 400 s each, back to back.
#   Read: cd switch/real_logs && python3 ../../tools/switchctl/por_escena.py bench_P02_* bench_P01_*  (and capturas.jpg)
set -uo pipefail
P=$HOME/nsmbw-port; R=$P/scripts/remoto.sh; C=$P/r9/r17pgo; Q=$P/tools/switchctl
echo "=== inicio $(date "+%F %T")"
while :; do
  rc=$($R estado 2>/dev/null | python3 -c "import json,sys;j=[x for x in json.load(sys.stdin)['trabajos'] if x['nombre']=='pgo_m2_fin'][0];print(j['rc'])" 2>/dev/null)
  [[ -n $rc ]] && break
  sleep 60
done
echo "$(date '+%F %T') compilación remota terminó con código $rc"
[[ $rc == 0 ]] || { $R log pgo_m2_fin 40; echo "FALLÓ: no sigo"; exit 1; }
rsync -a 'nsmbw-build:nsmbw-port/r9/r17pgo/bin/m2_pgo_m2_all*' "$C/bin/" || { echo "FALLÓ la copia de vuelta"; exit 1; }
ls -la "$C"/bin/m2_pgo_m2_all*
cd "$Q"
L='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo";NWII_FORCE_DEMO_LOOP="1"'
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$L" ./cola.sh add "$C/bin/m2_pgo_m2_all.nro" P01_m2pgo_lista 400
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$L" ./cola.sh add "$P/switch/real_builds/m2_1110/nsmbw_m2.nro" P02_m2_control 400
systemctl --user is-active -q nsmbw-cola || systemd-run --user --unit=nsmbw-cola --collect --quiet \
  -p StandardOutput=append:$Q/cola_run.log -p StandardError=append:$Q/cola_run.log $Q/cola.sh run
echo "$(date '+%F %T') P01/P02 en la cola (~20 min)"
