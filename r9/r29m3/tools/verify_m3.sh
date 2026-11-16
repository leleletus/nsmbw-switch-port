#!/usr/bin/env bash
# verify_m3.sh <name> [steps...]   Verificación en PC de bin/NSMBW_<name>_input_test de r29m3
# (copia de r24m2/tools/verify_m2.sh + los pasos de X1/D1; cada corrida del juego toma r9/render/pc/.runlock, NAND privada mp de render/pc).
#   checks : render/test/checks.sh a 1x (FRAMECHECK + ECACHE; title/map/walk/lvl11) + NWII_PINDEX_CHECK/NWII_DLMEMO_CHECK de R7
#   lista  : lista fija de demos (FORCE_DEMO + LOOP + NO_SAVE), 300 s, con NWII_NATIVE_STATS=1 y NWII_D1_STATS=1
#   vnative: la misma lista, 300 s, NWII_NATIVE_VERIFY=$VERIFY (2000) + NWII_AX_THREAD=0: cada nativa (X1 y D1) contra el generado
#   d1check: la misma lista, 300 s, NWII_D1_CHECK=1 + NWII_D1_STATS=1: índice contra el recorrido, ProfName contra el generado
#   sanity long cb4 title off : verify_g5_copy.sh
# Resultados: r29m3/runs/<paso>_<name>.txt (+ hojas de capturas)
set -u
M=/home/djvemo/nsmbw-port/r9/r29m3; R=/home/djvemo/nsmbw-port/r9/render
n=$1; shift
bin=$M/bin/NSMBW_${n}_input_test
lock() { flock "$R/pc/.runlock" "$@"; }
DEMOS=desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo
demo_run() {  # demo_run <paso> <segundos> [VAR=valor ...]
  local st=$1 secs=$2; shift 2
  local rn=m3_${st}_$n
  (cd $R && lock env NWII_FORCE_DEMO=$DEMOS NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 "$@" EVERY=300 PROFILE=free RES=1 \
     pc/run.sh "$bin" $rn $secs mp '0-330:100:20:4') > /dev/null 2>&1
  local out=$M/runs/${st}_$n L=$R/pc/runs/$rn/run.log; rm -rf "$out"; mkdir -p "$out"
  grep -E '^\[DEMO\]|\[SCENE\].*scene=' "$L" > "$out/demo.txt"
  grep -E '^\[D1\]|^\[N1\]|^\[FPS|FASTMEM\]|exit=|MISMATCH|Segmentation|abort' "$L" > "$out/natives.txt"
  cp $R/pc/runs/$rn/summary.txt "$out/"
  ls $R/pc/runs/$rn/t_*.png > /dev/null 2>&1 && \
    magick montage -label '%t' $(ls $R/pc/runs/$rn/t_*.png | sort -V) -tile 8x -geometry 240x180+2+2 "$out/hoja.jpg"
  { cat "$out/summary.txt"; grep '^\[DEMO\]' "$out/demo.txt"
    echo "MISMATCH lines: $(grep -c MISMATCH "$L")"
    grep -E '^\[N1\].*(final|total|on|OFF)|^\[D1\].*(final|total|CHECK|searchNodeByID:)' "$L" | tail -60
    grep '\[FASTMEM\]' "$L" | tail -1; tail -1 "$L"; } > $M/runs/${st}_$n.txt 2>&1
}
for st in "$@"; do
  case $st in
    checks)
      (cd $R && lock env RESS=1 NWII_PINDEX_CHECK=1 NWII_DLMEMO_CHECK=1 test/checks.sh "$bin" m3_$n) > $M/runs/checks_$n.txt 2>&1
      for sc in title map walk lvl11; do L=$R/pc/runs/chk_m3_${n}_${sc}_1x/run.log
        echo "$sc: $(grep '\[PINDEX_CHECK\]' $L | tail -1) | $(grep '\[DLMEMO_CHECK\]' $L | tail -1) | $(grep '\[FASTMEM\]' $L | tail -1) | $(tail -1 $L)"
      done >> $M/runs/checks_$n.txt ;;
    lista)   demo_run lista 300 NWII_NATIVE_STATS=1 NWII_D1_STATS=1 ;;
    vnative) demo_run vnative ${T_VN:-300} NWII_AX_THREAD=0 NWII_NATIVE_VERIFY=${VERIFY:-2000} ;;
    d1check) demo_run d1check ${T_D1:-300} NWII_D1_CHECK=1 NWII_D1_STATS=1 ;;
    sanity|long|cb4|title|off)
      BIN=$bin OUT=$M/runs TAG=m3_$n $M/tools/verify_g5_copy.sh $st > /dev/null 2>&1
      mv -f $M/runs/$st.txt $M/runs/${st}_$n.txt
      rn=g5_m3_${n}_$st
      ls $R/pc/runs/$rn/t_*.png > /dev/null 2>&1 && \
        magick montage -label '%t' $(ls $R/pc/runs/$rn/t_*.png | sort -V) -tile 8x -geometry 240x180+2+2 $M/runs/hoja_${st}_$n.jpg ;;
  esac
  echo "$st done: $M/runs/${st}_$n.txt"
done
