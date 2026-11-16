#!/usr/bin/env bash
# [r44w] encolar_w.sh <nro variante> <n0> <variantes...>: corridas del frente W, contiguas, al final de la cola, lista fija
# de 5 demos, 400 s, entorno base de M (m6e trae lo demás por defecto). Variantes: ctl (= el NRO de m6e, sin nada),
# agua (NWII_W_AGUA), wgp (NWII_W_WGP), ambos. Nombres W<nn>_<v>.
set -eu
nro=$1; n=$2; shift 2
Q=/home/djvemo/nsmbw-port/tools/switchctl; M6E=/home/djvemo/nsmbw-port/r9/r41m/bin/m6e_pgo.nro
L='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo";NWII_FORCE_DEMO_LOOP="1"'
add() { BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$L$3" $Q/cola.sh add "$1" "$(printf W%02d $n)_$2" 400; n=$((n + 1)); }
for v in "$@"; do
  case $v in
    ctl)   add $M6E m6e_ctl '' ;;
    agua)  add $nro w2_agua ';NWII_W_AGUA="1"' ;;
    wgp)   add $nro w2_wgp ';NWII_W_WGP="1"' ;;
    ambos) add $nro w2_ambos ';NWII_W_AGUA="1";NWII_W_WGP="1"' ;;
  esac
done
$Q/cola.sh list | grep '\.job$' | tail -6
