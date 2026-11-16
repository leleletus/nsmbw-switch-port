#!/usr/bin/env bash
# [r40d] encolar_d.sh <nro> <tag> <n0> [variantes="off on"]: A/B del frente D en la cola de la consola, MISMO binario,
# contiguas, lista fija, 400 s, con el entorno de V03 (NWII_V_WAKE=1 NWII_V_LATE=8; S2 por defecto). Nombres D<nn>_<tag>_<v>.
set -eu
nro=$1; p=$2; n=$3
Q=/home/djvemo/nsmbw-port/tools/switchctl
L='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo";NWII_FORCE_DEMO_LOOP="1";NWII_V_WAKE="1";NWII_V_LATE="8"'
add() { BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 DEBUG_EXTRA="$L$2" $Q/cola.sh add "$nro" "$(printf D%02d $n)_${p}_$1" 400; n=$((n + 1)); }
for v in ${4:-off on}; do
  case $v in
    off) add off '' ;;
    on)  add on ';NWII_D_DROPFAST="1"' ;;
    all) add all ';NWII_D_DROPFAST="1";NWII_D_PARSESTOP="1";NWII_D_LIGHTPARSE="1";NWII_D_FASTDECODE="1";NWII_D_EAGERDECODE="1"' ;;
    noeager) add noeager ';NWII_D_DROPFAST="1";NWII_D_PARSESTOP="1";NWII_D_LIGHTPARSE="1";NWII_D_FASTDECODE="1";NWII_D_FASTPREP="1"' ;;
    def) add def '' ;;
    rp) add rp ';NWII_D_DROPFAST="1";NWII_D_PARSESTOP="1";NWII_D_LIGHTPARSE="1";NWII_D_FASTDECODE="1";NWII_D_FASTPREP="1";NWII_D_RENDERPREP="1"' ;;
    p2) add p2 ';NWII_D_DROPFAST="1";NWII_D_PARSESTOP="1";NWII_D_LIGHTPARSE="1";NWII_D_FASTDECODE="1";NWII_D_FASTPREP="1"' ;;
    p2m) add p2m ';NWII_D_DROPFAST="1";NWII_D_PARSESTOP="1";NWII_D_LIGHTPARSE="1";NWII_D_FASTDECODE="1";NWII_D_FASTPREP="1";NWII_D_MERGE="1"' ;;
    rpm) add rpm ';NWII_D_DROPFAST="1";NWII_D_PARSESTOP="1";NWII_D_LIGHTPARSE="1";NWII_D_FASTDECODE="1";NWII_D_FASTPREP="1";NWII_D_RENDERPREP="1";NWII_D_MERGE="1"' ;;
    allp) add allp ';NWII_D_DROPFAST="1";NWII_D_PARSESTOP="1";NWII_D_LIGHTPARSE="1";NWII_D_FASTDECODE="1";NWII_D_EAGERDECODE="1";NWII_D_FASTPREP="1"' ;;
  esac
done
$Q/cola.sh list | grep '\.job$' | tail -14
