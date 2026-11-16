#!/usr/bin/env bash
# cadena_pgo_m2.sh: no-AI chain for PGO step 1->2 of m2.
#   waits for the remote build (scripts/remoto.sh job pgo_m2_gen) -> brings bin/m2_pgogen*.nro back ->
#   queues the two console training runs (forced demos 900 s + scripted gameplay 900 s, both NO_SAVE, each pulls
#   its .gcda into r9/r17pgo/profiles/raw via PULL_CMD) -> starts the console queue.
# Then (by hand): r9/r17pgo/pgo_release.sh finish m2 profiles/raw/<run1> profiles/raw/<run2>   (see status/PAUSA_1110.md)
set -uo pipefail
P=$HOME/nsmbw-port; R=$P/scripts/remoto.sh; C=$P/r9/r17pgo; Q=$P/tools/switchctl
echo "=== inicio $(date "+%F %T")"
echo "$(date '+%F %T') esperando la compilación remota pgo_m2_gen"
while :; do
  rc=$($R estado 2>/dev/null | python3 -c "import json,sys;j=[x for x in json.load(sys.stdin)['trabajos'] if x['nombre']=='pgo_m2_gen'][0];print(j['rc'])" 2>/dev/null)
  [[ -n $rc ]] && break
  sleep 60
done
echo "$(date '+%F %T') compilación remota terminó con código $rc"
[[ $rc == 0 ]] || { $R log pgo_m2_gen 30; echo "FALLÓ: no sigo"; exit 1; }
mkdir -p "$C/bin"
rsync -a nsmbw-build:nsmbw-port/r9/r17pgo/bin/m2_pgogen.nro nsmbw-build:nsmbw-port/r9/r17pgo/bin/m2_pgogen.elf \
  nsmbw-build:nsmbw-port/r9/r17pgo/bin/m2_pgogen_input_test.nro "$C/bin/" || { echo "FALLÓ la copia"; exit 1; }
rsync -a nsmbw-build:nsmbw-port/r9/r17pgo/nx/m2_pgogen "$C/nx/" || echo "aviso: no se copió nx/m2_pgogen (import lo usa para comprobar nombres)"
ls -la "$C"/bin/m2_pgogen*
cd "$Q"
L='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_GCOV_DUMP_EVERY="60";NWII_GCOV_DUMP_AT="880";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo,torre3,castillo6";NWII_FORCE_DEMO_LOOP="1"'
J='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_GCOV_DUMP_EVERY="60";NWII_GCOV_DUMP_AT="880";NWII_INPUT_SCRIPT="0-100000:100:20:4";NWII_INPUT_PHASES="3>5>3>5>3>5>3>5";NWII_INPUT_SCRIPT_1="0-100000:100:20:4";NWII_INPUT_SCRIPT_2="0-10000000:204:1:1;0-10000000:100:45:30;17-10000000:100:97:6";NWII_INPUT_SCRIPT_3="0-100000:4:120:8;60-100000:100:120:6";NWII_INPUT_SCRIPT_4="0-10000000:204:1:1;0-10000000:100:45:30;17-10000000:100:97:6";NWII_INPUT_SCRIPT_5="0-100000:4:120:8;60-100000:100:120:6";NWII_INPUT_SCRIPT_6="0-10000000:204:1:1;0-10000000:100:45:30;17-10000000:100:97:6";NWII_INPUT_SCRIPT_7="0-100000:4:120:8;60-100000:100:120:6";NWII_INPUT_SCRIPT_8="0-10000000:204:1:1;0-10000000:100:45:30;17-10000000:100:97:6"'
PULL="$C/tools/pull_gcda.sh $C/profiles/raw"
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=30 PULL_CMD="$PULL" DEBUG_EXTRA="$L" ./cola.sh add "$C/bin/m2_pgogen.nro" M01_m2gen_demos 900
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=30 PULL_CMD="$PULL" DEBUG_EXTRA="$J" ./cola.sh add "$C/bin/m2_pgogen_input_test.nro" M02_m2gen_juego 900
systemctl --user is-active -q nsmbw-cola || systemd-run --user --unit=nsmbw-cola --collect --quiet \
  -p StandardOutput=append:$Q/cola_run.log -p StandardError=append:$Q/cola_run.log $Q/cola.sh run
echo "$(date '+%F %T') entrenamiento en la cola de la Switch (~35 min). Después: pgo_release.sh finish m2 ..."
