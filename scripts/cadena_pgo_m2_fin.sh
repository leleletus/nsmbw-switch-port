#!/usr/bin/env bash
# cadena_pgo_m2_fin.sh: no-AI chain, PGO step 3 of m2.
#   waits for M01+M02 (console queue) to finish with rc 0 -> takes the 2 run folders in r9/r17pgo/profiles/raw newer
#   than profiles/.marca_1112 -> copies profiles to the other laptop -> there: pgo_release.sh finish m2 (JOBS=11)
#   -> brings bin/m2_pgo_m2_all*.nro back. Next (by hand): measure against m2 (see status/PAUSA_1110.md step 4).
set -uo pipefail
P=$HOME/nsmbw-port; R=$P/scripts/remoto.sh; C=$P/r9/r17pgo; H=$P/tools/switchctl/cola_hecho.txt
echo "=== inicio $(date "+%F %T")"
n0=$(wc -l < "$H")
while :; do
  nuevas=$(tail -n +$((n0 + 1)) "$H")
  grep -q 'COLA DETENIDA' <<< "$nuevas" && { echo "$nuevas"; echo "FALLÓ: la cola se detuvo, no sigo"; exit 1; }
  ok=$(grep -cE 'M0[12]_m2gen_[a-z]+ \(rc 0\)' <<< "$nuevas")
  ((ok >= 2)) && break
  sleep 60
done
echo "$(date '+%F %T') entrenamiento terminado"; echo "$nuevas"
mapfile -t runs < <(cd "$C" && find profiles/raw -mindepth 1 -maxdepth 1 -type d -newer profiles/.marca_1112 | sort)
echo "carpetas: ${runs[*]}"
((${#runs[@]} >= 2)) || { echo "FALLÓ: esperaba 2 carpetas de perfiles nuevas"; exit 1; }
for r in "${runs[@]}"; do [[ -f $C/$r/dumps.txt ]] || echo "aviso: $r sin dumps.txt"; ls "$C/$r" | grep -c gcda; done
$R push r9/r17pgo/profiles || { echo "FALLÓ la copia"; exit 1; }
$R run pgo_m2_fin "cd r9/r17pgo && JOBS=11 ./pgo_release.sh finish m2 ${runs[*]}"
while :; do
  rc=$($R estado 2>/dev/null | python3 -c "import json,sys;j=[x for x in json.load(sys.stdin)['trabajos'] if x['nombre']=='pgo_m2_fin'][0];print(j['rc'])" 2>/dev/null)
  [[ -n $rc ]] && break
  sleep 60
done
echo "$(date '+%F %T') compilación remota terminó con código $rc"
[[ $rc == 0 ]] || { $R log pgo_m2_fin 30; echo "FALLÓ: no sigo"; exit 1; }
rsync -a 'nsmbw-build:nsmbw-port/r9/r17pgo/bin/m2_pgo_m2_all*' "$C/bin/" || { echo "FALLÓ la copia de vuelta"; exit 1; }
rsync -a nsmbw-build:nsmbw-port/r9/r17pgo/profiles/m2_all "$C/profiles/" || true
ls -la "$C"/bin/m2_pgo_m2_all*
echo "$(date '+%F %T') LISTO: bin/m2_pgo_m2_all.nro. Siguiente: medir contra m2 (PAUSA_1110.md paso 4)"
