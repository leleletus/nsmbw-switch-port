#!/usr/bin/env bash
# audio_run.sh <binary> <name> <secs> [extra env words...]
#   PC run of the title screen + attract demos with no input (plain game binary), private NAND copy
#   (r9/render/pc/nand_tpl_mp), offscreen, audio written to runs/<name>/audio.wav (NWII_AUDIODUMP).
#   Deterministic mode for audio comparisons: DET=1 -> NWII_REALTIME=0 (virtual clock) +
#   NWII_AX_THREAD=0 (mix inline) + NWII_NOFRAMESKIP=1. Run it under
#   flock r9/r43a/pc/.runlock (copia de r35m4/tools/audio_run.sh con el arnés de r41m) (shared GPU / timing).
set -u
R=/home/djvemo/nsmbw-port/r9/r43a/pc
V=/home/djvemo/nsmbw-port/r9/r43a/pc
bin=$(realpath "$1"); name=$2; secs=$3; shift 3
out=$V/runs/$name; rm -rf -- "$out"; mkdir -p "$out"
w=$V/runs/work_$name; rm -rf -- "$w"; mkdir -p "$w"; cd "$w" || exit 1
cp -a "$R/nand_tpl_mp" ./nand
cp "$R/shadercache.tpl" ./shadercache.bin
cp "$R/default.toml" ./nsmbw.toml
sed -i "s#^nand_dir = .*#nand_dir = \"$w/nand\"#; s#^output_dir = .*#output_dir = \"$w/unused\"#" ./nsmbw.toml
det=(); [[ -n ${DET:-} ]] && det=(NWII_REALTIME=0 NWII_AX_THREAD=0 NWII_NOFRAMESKIP=1)
env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy NWII_FPS=1 NWII_AUDIODUMP="$out/audio.wav" "${det[@]}" "$@" \
  PROFILE=${PROFILE:-free} $R/limc timeout "$secs" stdbuf -oL -eL "$bin" \
  /home/djvemo/nsmbw-port/game/SMNE01 nsmbw.toml > "$out/run.log" 2>&1
echo "exit=$?" >> "$out/run.log"
cd "$V" && rm -rf -- "$w"
grep -o 'scene=[0-9]* ([A-Z_]*)' "$out/run.log" | awk '{print $2}' | uniq | tr '\n' ' ' > "$out/scenes.txt"
echo >> "$out/scenes.txt"
grep '^\[FPS' "$out/run.log" | awk '{s+=$2; n++} END{if(n) printf "fps avg=%.1f n=%d\n", s/n, n}' >> "$out/scenes.txt"
grep -E '^\[A2\]|^\[R9\] axfx' "$out/run.log" | tail -3 >> "$out/scenes.txt"
ls -la "$out/audio.wav" >> "$out/scenes.txt" 2>&1
cat "$out/scenes.txt"
