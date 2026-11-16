#!/usr/bin/env bash
# pgo_g7.sh <nombre> <export> <runtime> [GEN_FLAGS]   (frente G7, sin IA; copia de los pasos de r17pgo para m3:
#   pgo_release.sh prep hecho a mano + scripts de r31t/tools/cadena_pgo_m3.sh y scripts/cadena_m3_fin.sh)
#   1. foto del arbol en r9/r17pgo/<nombre>src (export + runtime, SHA256SUMS) y conf/<nombre>.env (con GEN_FLAGS,
#      p. ej. -DG7_NOIC: va igual en la instrumentada y en la optimizada);
#   2. instrumentada en la otra laptop (CONF=<nombre> JOBS=11 ./pgo.sh gen) y la trae;
#   3. entrenamiento en la consola SOLO con demos (dos corridas de 900 s, como m3: listas A y B), NO_SAVE, gl;
#   4. lleva los perfiles a la otra laptop, pgo_release.sh finish <nombre> (JOBS=11), trae bin/<nombre>_pgo_*;
#   5. encola la medida: <P1> = <nombre>+PGO y <P2> = m3 (r17pgo/bin/m3_pgo_m3_all.nro), lista fija, 400 s.
# Registro: r9/r33g7/runs/pgo_<nombre>.log. No toca los archivos de m2/m3 de r17pgo.
set -uo pipefail
N=${1:?nombre}; EXP=$(realpath "${2:?export}"); RTD=$(realpath "${3:?runtime}"); GF=${4:-}
P=$HOME/nsmbw-port; R=$P/scripts/remoto.sh; C=$P/r9/r17pgo; Q=$P/tools/switchctl; H=$Q/cola_hecho.txt
TAG=${TAG:-Y}
exec >> $P/r9/r33g7/runs/pgo_$N.log 2>&1
log() { echo "[$(date '+%F %T')] $*"; }
esperar_remoto() {  # <trabajo>
  local rc
  while :; do
    rc=$($R estado 2>/dev/null | python3 -c "import json,sys;j=[x for x in json.load(sys.stdin)['trabajos'] if x['nombre']=='$1'];print(j[0]['rc'] if j else '')" 2>/dev/null)
    [[ -n $rc ]] && break; sleep 60
  done
  log "remoto $1 rc=$rc"; [[ $rc == 0 ]] || { $R log "$1" 30; log "FALLO $1"; exit 1; }
}
S=$C/${N}src
if [[ ! -f $S/SHA256SUMS ]]; then
  log "prep $N: export $EXP runtime $RTD GEN_FLAGS='$GF'"
  [[ -e $S ]] && { log "$S existe sin SHA256SUMS"; exit 2; }
  mkdir -p "$S/export" "$S/lib"
  cp -a --reflink=auto "$RTD" "$S/nWiiRuntime"
  (cd "$EXP" && ls -A | grep -v '^build$\|^nWiiRuntime$\|\.o$' | xargs -I{} cp -a --reflink=auto {} "$S/export/")
  (cd "$S" && find export nWiiRuntime -type f | sort | xargs sha256sum > SHA256SUMS)
  printf 'export %s\nruntime %s\nGEN_FLAGS %s\ndate %s\n(pgo_g7.sh, frente G7)\n' "$EXP" "$RTD" "$GF" "$(date '+%F %T')" > "$S/ORIGEN.txt"
  cp -a "$P/r9/r15dk/build/uamlib_nx/libuamlib_ns.a" "$S/lib/"
  cat > "$C/conf/$N.env" <<EOF
# conf/$N.env (r9/r33g7/tools/pgo_g7.sh, $(date '+%F %T')): snapshot of $EXP + $RTD
TREE=$S
EXTRA_LIBS="$S/lib/libuamlib_ns.a /opt/devkitpro/libnx/lib/libdeko3d.a"
FMCHECK=$C/tools/check_fm_tab_g5.py
GENV=${N}_pgogen
GEN_FLAGS="$GF"
EOF
fi
if [[ ! -f $C/bin/${N}_pgogen.nro ]]; then
  $R push r9/r17pgo/${N}src r9/r17pgo/conf || { log "FALLO push"; exit 1; }
  $R run pgo_${N}_gen "cd r9/r17pgo && CONF=$N JOBS=${PJOBS:-8} ./pgo.sh gen"
  esperar_remoto pgo_${N}_gen
  mkdir -p "$C/bin" "$C/nx"
  rsync -a nsmbw-build:nsmbw-port/r9/r17pgo/bin/${N}_pgogen.nro nsmbw-build:nsmbw-port/r9/r17pgo/bin/${N}_pgogen.elf "$C/bin/" || { log "FALLO copia"; exit 1; }
  rsync -a nsmbw-build:nsmbw-port/r9/r17pgo/nx/${N}_pgogen "$C/nx/" || log "aviso: sin nx/${N}_pgogen"
fi
mapfile -t runs < <(cd "$C/profiles/raw" && for d in */; do d=${d%/}; ls "$d" | grep -q "#r17pgo#${N}src#" && [[ -f $d/dumps.txt ]] && echo "$d"; done)
if ((${#runs[@]} < 2)); then
  cd "$Q"
  n0=$(wc -l < "$H")
  G='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_GCOV_DUMP_EVERY="60";NWII_GCOV_DUMP_AT="880";NWII_FORCE_DEMO_LOOP="1"'
  A="$G;NWII_FORCE_DEMO=\"desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo,torre3,castillo6\""
  B="$G;NWII_FORCE_DEMO=\"nieve,casa_oscura,lava,jungla,playa_yoshi,playa_hielo,desierto_helices,pradera,cielo_hoja\""
  PULL="$C/tools/pull_gcda.sh $C/profiles/raw"
  BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=30 PULL_CMD="$PULL" DEBUG_EXTRA="$A" ./cola.sh add "$C/bin/${N}_pgogen.nro" ${TAG}01_${N}gen_demosA 900
  sleep 1
  BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=30 PULL_CMD="$PULL" DEBUG_EXTRA="$B" ./cola.sh add "$C/bin/${N}_pgogen.nro" ${TAG}02_${N}gen_demosB 900
  log "entrenamiento encolado (${TAG}01/${TAG}02)"
  while :; do
    nuevas=$(tail -n +$((n0 + 1)) "$H")
    grep -q 'COLA DETENIDA' <<< "$nuevas" && log "aviso: la cola se detuvo: $(grep 'COLA DETENIDA' <<< "$nuevas" | tail -1)"
    (( $(grep -cE "${TAG}0[12]_${N}gen_demos[AB] \(rc 0\)" <<< "$nuevas") >= 2 )) && break
    sleep 60
  done
  log "entrenamiento terminado"
  mapfile -t runs < <(cd "$C/profiles/raw" && for d in */; do d=${d%/}; ls "$d" | grep -q "#r17pgo#${N}src#" && [[ -f $d/dumps.txt ]] && echo "$d"; done)
fi
log "carpetas: ${runs[*]}"
((${#runs[@]} >= 2)) || { log "FALLO: esperaba 2 carpetas de perfiles de ${N}src"; exit 1; }
rr=(); for r in "${runs[@]}"; do log "$r: $(ls "$C/profiles/raw/$r" | grep -c gcda) .gcda"; rr+=("profiles/raw/$r"); done
if [[ ! -f $C/bin/${N}_pgo_${N}_all.nro ]]; then
  $R push "${rr[@]/#/r9/r17pgo/}" || { log "FALLO push perfiles"; exit 1; }
  $R run pgo_${N}_fin "cd r9/r17pgo && JOBS=11 ./pgo_release.sh finish $N ${rr[*]}"
  esperar_remoto pgo_${N}_fin
  rsync -a "nsmbw-build:nsmbw-port/r9/r17pgo/bin/${N}_pgo_${N}_all*" "$C/bin/" || { log "FALLO copia de vuelta"; exit 1; }
fi
ls -la "$C"/bin/${N}_pgo_${N}_all*
python3 $C/tools/check_fm_tab_g5.py "$C/bin/${N}_pgo_${N}_all.elf" 2>&1 | tail -2 || true
cd "$Q"
L='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo";NWII_FORCE_DEMO_LOOP="1"'
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$L" ./cola.sh add "$C/bin/${N}_pgo_${N}_all.nro" ${TAG}03_${N}pgo_lista 400
sleep 1
BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$L" ./cola.sh add "$C/bin/m3_pgo_m3_all.nro" ${TAG}04_m3pgo_lista 400
cp -f "$C/bin/${N}_pgo_${N}_all.elf" "$P/r9/r33g7/bin/${N}_pgo_${TAG}03.elf"
log "medida encolada: ${TAG}03 (${N}+PGO), ${TAG}04 (m3+PGO). Leer: python3 r9/r31t/tools/por_escena_real.py switch/real_logs/bench_${TAG}04_* switch/real_logs/bench_${TAG}03_*"
