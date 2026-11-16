#!/usr/bin/env bash
# run_dx.sh <binario> <nombre> <segundos> [VAR=valor ...]
#   Corrida de PC sin ventana ni sonido (copia privada del arnés: r32dx/pc/run.sh, NAND mp) con la lista fija de
#   demos, NWII_NO_SAVE y NWII_DX=<r32dx/runs/<nombre>/dx> (contadores por escena; con el binario gcov, los .gcda
#   de cada segmento). Toma el .runlock común de r9/render/pc (una sola corrida del juego a la vez en esta PC).
#   PROFILE=free|p2|sw|sw1|e2 (ver pc/limc). Deja r32dx/runs/<nombre>/{run.log,dx/,dx.txt,hoja.jpg}.
set -u
V=/home/djvemo/nsmbw-port/r9/r32dx
bin=$(realpath "$1"); n=$2; secs=$3; shift 3
out=$V/runs/$n; rm -rf "$out"; mkdir -p "$out/dx"
LISTA=${LISTA:-desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas,castillo}
(cd $V && flock /home/djvemo/nsmbw-port/r9/render/pc/.runlock env NWII_FORCE_DEMO="$LISTA" NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 \
   NWII_DX="$out/dx" "$@" EVERY=${EVERY:-600} PROFILE=${PROFILE:-free} RES=1 pc/run.sh "$bin" dx_$n "$secs" mp '0-330:100:20:4') > "$out/harness.log" 2>&1
R=$V/pc/runs/dx_$n
cp "$R/run.log" "$out/run.log"; cp "$R/summary.txt" "$out/" 2>/dev/null
grep -E '^\[DX\]|^\[DEMO\]' "$out/run.log" > "$out/dx.txt"
grep -E '^\[FPS\]' "$out/run.log" > "$out/fps.txt"
ls $R/t_*.png > /dev/null 2>&1 && magick montage -label '%t' $(ls $R/t_*.png | sort -V) -tile 8x -geometry 240x180+2+2 "$out/hoja.jpg"
rm -rf "$R"
grep -c '^\[DX\] fin' "$out/dx.txt"; tail -3 "$out/summary.txt"
