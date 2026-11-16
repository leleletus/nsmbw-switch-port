#!/usr/bin/env bash
# d1_run.sh <binario_input_test> <nombre> <segundos> [VAR=valor ...]
#   r28d1: copia de r21s/tools/demo_run.sh (render/pc/run.sh, NAND privada mp, sin ventana ni sonido; toma
#   .runlock). Deja en r28d1/runs/<nombre>/: run.log (líneas [D1]/[N1]/[DEMO]/[FPS]), demo.txt, summary.txt, hoja.jpg
set -u
D=/home/djvemo/nsmbw-port/r9/r28d1; R=/home/djvemo/nsmbw-port/r9/render
bin=$(realpath "$1"); n=$2; secs=$3; shift 3
(cd $R && flock pc/.runlock env "$@" EVERY=${EVERY:-600} PROFILE=${PROFILE:-free} RES=1 pc/run.sh "$bin" d1_$n "$secs" mp '0-330:100:20:4') > /dev/null 2>&1
out=$D/runs/$n; rm -rf "$out"; mkdir -p "$out"
L=$R/pc/runs/d1_$n/run.log
grep -E '^\[DEMO\]|\[SCENE\].*scene=' "$L" > "$out/demo.txt"
grep -E '^\[D1\]|^\[N1\]|^\[FPS|FASTMEM\]|exit=' "$L" > "$out/d1.txt"
cp $R/pc/runs/d1_$n/summary.txt "$out/"
ls $R/pc/runs/d1_$n/t_*.png > /dev/null 2>&1 && \
  magick montage -label '%t' $(ls $R/pc/runs/d1_$n/t_*.png | sort -V) -tile 8x -geometry 240x180+2+2 "$out/hoja.jpg"
echo "$out"
