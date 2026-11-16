#!/usr/bin/env bash
# pcrun.sh <binary> <name> <secs> [env...]: corrida de PC (copia de r35m4/tools/audio_run.sh SIN volcado de audio),
# offscreen, NAND privada. Salida en runs/<name>/run.log. Correr bajo flock pc/.runlock (copia de r38c/tools/pcrun.sh, arnés propio).
set -u
R=/home/djvemo/nsmbw-port/r9/r41m/pc
V=$(cd "$(dirname "$0")/.." && pwd)
bin=$(realpath "$1"); name=$2; secs=$3; shift 3
out=$V/pc/runs/$name; rm -rf -- "$out"; mkdir -p "$out"
w=$V/pc/runs/work_$name; rm -rf -- "$w"; mkdir -p "$w"; cd "$w" || exit 1
cp -a "$R/nand_tpl_mp" ./nand
cp "$R/shadercache.tpl" ./shadercache.bin
cp "$R/default.toml" ./nsmbw.toml
sed -i "s#^nand_dir = .*#nand_dir = \"$w/nand\"#; s#^output_dir = .*#output_dir = \"$w/unused\"#" ./nsmbw.toml
env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy NWII_FPS=1 NWII_REALTIME=1 "$@" \
  PROFILE=${PROFILE:-free} $R/limc timeout -k 10 "$secs" stdbuf -oL -eL "$bin" \
  /home/djvemo/nsmbw-port/game/SMNE01 nsmbw.toml > "$out/run.log" 2>&1
echo "exit=$?" >> "$out/run.log"
cd "$V" && rm -rf -- "$w"
