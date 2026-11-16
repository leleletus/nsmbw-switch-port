#!/usr/bin/env bash
# cadena_m3_fin.sh: no-AI. Waits for V01+V02 (m3 PGO training on the console) -> checks the 2 new profile folders ->
# copies them to the other laptop -> pgo_release.sh finish m3 (JOBS=11) -> brings bin/m3_pgo_m3_all* back -> queues
# X01 (m3+PGO) and X02 (t1 control), fixed list, 400 s -> then puts back the poll sweep jobs W01-W03.
# Read: cd switch/real_logs && python3 ../../r9/r31t/tools/por_escena_real.py bench_X02_* bench_X01_*   (+ capturas.jpg)
set -uo pipefail
P=$HOME/nsmbw-port; R=$P/scripts/remoto.sh; C=$P/r9/r17pgo; Q=$P/tools/switchctl; H=$Q/cola_hecho.txt
echo "=== inicio $(date "+%F %T")"
n0=${N0:-$(wc -l < "$H")}
while :; do
  nuevas=$(tail -n +$((n0 + 1)) "$H")
  grep -q 'COLA DETENIDA' <<< "$nuevas" && { echo "$nuevas"; echo "FALLÓ: la cola se detuvo, no sigo"; exit 1; }
  (( $(grep -cE 'V0[12]_m3gen_demos[AB] \(rc 0\)' <<< "$nuevas") >= 2 )) && break
  sleep 60
done
echo "$(date '+%F %T') entrenamiento terminado"; echo "$nuevas"
mapfile -t runs < <(cd "$C" && find profiles/raw -mindepth 1 -maxdepth 1 -type d -newer profiles/.marca_m3 ! -name '.*' | sort)
echo "carpetas: ${runs[*]}"
((${#runs[@]} == 2)) || { echo "FALLÓ: esperaba 2 carpetas de perfiles nuevas"; exit 1; }
for r in "${runs[@]}"; do
  [[ -f $C/$r/dumps.txt ]] || { echo "FALLÓ: $r sin dumps.txt (perfil incompleto)"; exit 1; }
  echo "$r: $(ls "$C/$r" | grep -c gcda) .gcda"
done
$R push "${runs[@]/#/r9/r17pgo/}" || { echo "FALLÓ la copia"; exit 1; }
$R run pgo_m3_fin "cd r9/r17pgo && JOBS=11 ./pgo_release.sh finish m3 ${runs[*]}"
while :; do
  rc=$($R estado 2>/dev/null | python3 -c "import json,sys;j=[x for x in json.load(sys.stdin)['trabajos'] if x['nombre']=='pgo_m3_fin'][0];print(j['rc'])" 2>/dev/null)
  [[ -n $rc ]] && break
  sleep 60
done
echo "$(date '+%F %T') compilación remota terminó con código $rc"
[[ $rc == 0 ]] || { $R log pgo_m3_fin 40; echo "FALLÓ: no sigo"; exit 1; }
rsync -a 'nsmbw-build:nsmbw-port/r9/r17pgo/bin/m3_pgo_m3_all*' "$C/bin/" || { echo "FALLÓ la copia de vuelta"; exit 1; }
ls -la "$C"/bin/m3_pgo_m3_all*
cd "$Q"
L='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo";NWII_FORCE_DEMO_LOOP="1"'
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$L" ./cola.sh add "$C/bin/m3_pgo_m3_all.nro" X01_m3pgo_lista 400
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$L" ./cola.sh add "$P/r9/r31t/bin/nsmbw_t1.nro" X02_t1_control 400
mv .cola/pausa_1112/*_W0*.job .cola/ 2>/dev/null
systemctl --user is-active -q nsmbw-cola || systemd-run --user --unit=nsmbw-cola --collect --quiet \
  -p StandardOutput=append:$Q/cola_run.log -p StandardError=append:$Q/cola_run.log $Q/cola.sh run
echo "$(date '+%F %T') X01/X02 + W01-W03 en la cola (~45 min)"
