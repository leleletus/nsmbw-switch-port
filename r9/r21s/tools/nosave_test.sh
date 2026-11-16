#!/usr/bin/env bash
# nosave_test.sh <binario_input_test> <plantilla: fresh|mp> <seg> [VAR=valor...]
#   r21s: comprueba NWII_NO_SAVE en PC. Dentro del mismo .runlock: corre el juego con NWII_NO_SAVE=1 y,
#   antes de soltar el candado, compara la NAND "real" de la corrida (render/pc/run/nand) con su plantilla
#   y lista lo escrito en la de prueba (run/nand_prueba, que luego se borra).
set -u
S=/home/djvemo/nsmbw-port/r9/r21s; R=/home/djvemo/nsmbw-port/r9/render
bin=$(realpath "$1"); tpl=$2; secs=$3; shift 3
out=$S/runs/nosave_$tpl; mkdir -p $out
cd $R && flock pc/.runlock bash -c '
  env "$@" NWII_NO_SAVE=1 EVERY=0 PROFILE=free RES=1 pc/run.sh '"$bin"' s21_nosave_'"$tpl"' '"$secs"' '"$tpl"' "${SCRIPT:-0-330:100:20:4}" > /dev/null 2>&1
  { echo "== NAND real (run/nand) frente a la plantilla nand_tpl_'"$tpl"' (vacío = sin cambios):"
    diff -rq pc/nand_tpl_'"$tpl"' pc/run/nand | grep -v "Only in pc/nand_tpl_'"$tpl"'/tmp"
    echo "== NAND de prueba (run/nand_prueba) frente a la plantilla:"
    diff -rq pc/nand_tpl_'"$tpl"' pc/run/nand_prueba
    echo "== líneas [FS] de escritura:"
    grep -E "^\[FS\] (NWII_NO_SAVE|create|delete|rename|open .*mode=[23])" pc/runs/s21_nosave_'"$tpl"'/run.log | sort | uniq -c
  } > '"$out"'/resultado.txt 2>&1
  rm -rf pc/run/nand_prueba' _ "$@"
cat $out/resultado.txt
