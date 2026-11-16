#!/usr/bin/env bash
# cadena_pgo_m3.sh (frente T, copia de scripts/cadena_pgo_m2.sh): espera la compilacion remota pgo_m3_gen ->
# trae bin/m3_pgogen*.nro y nx/m3_pgogen -> encola en la consola las dos corridas de entrenamiento (solo demos
# forzadas, 900 s, NO_SAVE, gl; cada una baja sus .gcda a r9/r17pgo/profiles/raw con PULL_CMD) -> arranca la cola.
# Despues (otra laptop): cd r9/r17pgo && JOBS=11 ./pgo_release.sh finish m3 profiles/raw/<A> profiles/raw/<B>
set -uo pipefail
P=$HOME/nsmbw-port; R=$P/scripts/remoto.sh; C=$P/r9/r17pgo; Q=$P/tools/switchctl
echo "=== inicio $(date "+%F %T")"
while :; do
  rc=$($R estado 2>/dev/null | python3 -c "import json,sys;j=[x for x in json.load(sys.stdin)['trabajos'] if x['nombre']=='pgo_m3_gen'][0];print(j['rc'])" 2>/dev/null)
  [[ -n $rc ]] && break
  sleep 30
done
echo "$(date '+%F %T') compilacion remota termino con codigo $rc"
[[ $rc == 0 ]] || { $R log pgo_m3_gen 30; echo "FALLO: no sigo"; exit 1; }
mkdir -p "$C/bin"
rsync -a nsmbw-build:nsmbw-port/r9/r17pgo/bin/m3_pgogen.nro nsmbw-build:nsmbw-port/r9/r17pgo/bin/m3_pgogen.elf \
  nsmbw-build:nsmbw-port/r9/r17pgo/bin/m3_pgogen_input_test.nro "$C/bin/" || { echo "FALLO la copia"; exit 1; }
rsync -a nsmbw-build:nsmbw-port/r9/r17pgo/nx/m3_pgogen "$C/nx/" || echo "aviso: no se copio nx/m3_pgogen"
ls -la "$C"/bin/m3_pgogen*
cd "$Q"
G='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_GCOV_DUMP_EVERY="60";NWII_GCOV_DUMP_AT="880";NWII_FORCE_DEMO_LOOP="1"'
A="$G;NWII_FORCE_DEMO=\"desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo,torre3,castillo6\""
B="$G;NWII_FORCE_DEMO=\"nieve,casa_oscura,lava,jungla,playa_yoshi,playa_hielo,desierto_helices,pradera,cielo_hoja\""
PULL="$C/tools/pull_gcda.sh $C/profiles/raw"
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=30 PULL_CMD="$PULL" DEBUG_EXTRA="$A" ./cola.sh add "$C/bin/m3_pgogen.nro" V01_m3gen_demosA 900
sleep 1
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=30 PULL_CMD="$PULL" DEBUG_EXTRA="$B" ./cola.sh add "$C/bin/m3_pgogen.nro" V02_m3gen_demosB 900
systemctl --user is-active -q nsmbw-cola || systemd-run --user --unit=nsmbw-cola --collect --quiet \
  -p StandardOutput=append:$Q/cola_run.log -p StandardError=append:$Q/cola_run.log $Q/cola.sh run
echo "$(date '+%F %T') entrenamiento encolado"
