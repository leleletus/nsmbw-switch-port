#!/usr/bin/env bash
# encolar_v.sh <nro> <prefijo> [primer numero=1]: nombres V<nn>_<prefijo>_<variante>; A/B del frente V en la cola de la consola, MISMO binario, contiguas, lista fija.
#   <prefijo>_off  (= m5), _wake (NWII_V_WAKE), _late8 (WAKE + LATE=8), _late12, _tl_off y _tl_late8 (linea de tiempo)
set -eu
nro=$1; p=$2; n=${3:-1}
Q=/home/djvemo/nsmbw-port/tools/switchctl
L='NWII_BACKEND="gl";NWII_NO_SAVE="1";NWII_FORCE_DEMO="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo";NWII_FORCE_DEMO_LOOP="1"'
add() { local name=$1 extra=$2 pull=${3:-}; BURST_FROM=20 BURST_SECS=0 SHOT_EVERY=10 PULL_PREFIX=$pull DEBUG_EXTRA="$L$extra" $Q/cola.sh add "$nro" "$(printf V%02d $n)_${p}_$name" 400; n=$((n + 1)); }
for v in ${VARIANTES:-off wake late8 late12 tl_off tl_late8}; do
  case $v in
    off)      add off '' ;;
    wake)     add wake ';NWII_V_WAKE="1"' ;;
    late8)    add late8 ';NWII_V_WAKE="1";NWII_V_LATE="8"' ;;
    late12)   add late12 ';NWII_V_WAKE="1";NWII_V_LATE="12"' ;;
    late8df)  add late8df ';NWII_V_WAKE="1";NWII_V_LATE="8";NWII_V_DROPFAST="1"' ;;
    wakedf)   add wakedf ';NWII_V_WAKE="1";NWII_V_DROPFAST="1"' ;;
    tl_off)   add tl_off ";NWII_V_TL=\"sdmc:/switch/NSMBWnx/logs/vtl_${p}_off.bin\"" vtl_ ;;
    tl_late8) add tl_late8 ";NWII_V_WAKE=\"1\";NWII_V_LATE=\"8\";NWII_V_TL=\"sdmc:/switch/NSMBWnx/logs/vtl_${p}_late8.bin\"" vtl_ ;;
  esac
done
$Q/cola.sh list | tail -12
