#!/usr/bin/env bash
# demo_run.sh <binario_input_test> <nombre> <segundos> [VAR=valor ...]
#   r21s: corrida de PC (r9/render/pc/run.sh, NAND privada mp, sin ventana ni sonido) de la pantalla de
#   título y sus demos, con las variables dadas (p. ej. NWII_FORCE_DEMO=desierto4j). Toma .runlock.
#   Deja en r21s/runs/<nombre>/: demo.txt (líneas [DEMO] + escenas), hoja.jpg (capturas cada EVERY frames).
set -u
S=/home/djvemo/nsmbw-port/r9/r21s; R=/home/djvemo/nsmbw-port/r9/render
bin=$(realpath "$1"); n=$2; secs=$3; shift 3
(cd $R && flock pc/.runlock env "$@" EVERY=${EVERY:-300} PROFILE=free RES=1 pc/run.sh "$bin" s21_$n "$secs" mp '0-330:100:20:4') > /dev/null 2>&1
out=$S/runs/$n; rm -rf "$out"; mkdir -p "$out"
L=$R/pc/runs/s21_$n/run.log
grep -E '^\[DEMO\]|\[SCENE\].*scene=' "$L" > "$out/demo.txt"
cp $R/pc/runs/s21_$n/summary.txt "$out/"
grep -m3 -E 'FASTMEM\]|exit=' "$L" >> "$out/summary.txt"
ls $R/pc/runs/s21_$n/t_*.png > /dev/null 2>&1 && \
  magick montage -label '%t' $(ls $R/pc/runs/s21_$n/t_*.png | sort -V) -tile 8x -geometry 240x180+2+2 "$out/hoja.jpg"
echo "$out"
