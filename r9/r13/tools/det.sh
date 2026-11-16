#!/usr/bin/env bash
# det.sh <binary_input_test> <name> <map|lvl11|title> <secs> [every=5]
#   Deterministic render check (r9:render): virtual guest clock (NWII_REALTIME=0) + no frame-skip
#   (NWII_NOFRAMESKIP=1) + every XFB copy dumped (NWII_XFBDUMP_ALL=1, every n-th guest copy), so the
#   same binary gives the same frames run after run; two binaries are compared frame by frame with
#   hashes.py / match.py. env: RES (internal res, default 1), SCALED=1, PROFILE (default free), EXTRA (env words)
set -u
T=$(cd "$(dirname "$0")/.." && pwd)
bin=$(realpath "$1"); name=$2; scene=$3; secs=$4; every=${5:-5}
export NWII_INPUT_SCRIPT_1= NWII_INPUT_SCRIPT_2=
case $scene in
  title) export NWII_INPUT_PHASES=6; scr='0-100000:100:20:4' ;;
  map)   export NWII_INPUT_PHASES=3; scr='0-100000:100:20:4' ;;
  lvl11) export NWII_INPUT_PHASES='3>5' NWII_INPUT_SCRIPT_1='0-100000:100:20:4' \
                NWII_INPUT_SCRIPT_2='0-10000000:100:120:4;60-10000000:4:120:12'; scr='0-100000:100:20:4' ;;
  *) echo "scene?" >&2; exit 2 ;;
esac
out=$T/test/runs/$name; rm -rf -- "$out"; mkdir -p "$out"
w=$T/test/work_$name; rm -rf -- "$w"; mkdir -p "$w"; cd "$w" || exit 1
cp -a "$T/pc/nand_tpl_mp" ./nand
cp "$T/pc/shadercache.tpl" ./shadercache.bin
cp "$T/pc/default.toml" ./nsmbw.toml
sed -i "s#^nand_dir = .*#nand_dir = \"$w/nand\"#; s#^output_dir = .*#output_dir = \"$w/unused\"#" ./nsmbw.toml
ex=(); [[ -n ${RES:-} ]] && ex+=(NWII_INTERNAL_RES=$RES); [[ -n ${SCALED:-} ]] && ex+=(NWII_DUMP_SCALED=1)
env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy NWII_FPS=1 NWII_REALTIME=0 NWII_NOFRAMESKIP=1 \
  NWII_XFBDUMP=$out/t NWII_XFBDUMP_ALL=1 NWII_XFBDUMP_EVERY=$every NWII_INTERNAL_RES=${RES:-1} "${ex[@]}" ${EXTRA:-} \
  NWII_INPUT_SCRIPT="$scr" PROFILE=${PROFILE:-free} $T/pc/limc timeout "$secs" stdbuf -oL -eL "$bin" \
  /home/djvemo/nsmbw-port/game/SMNE01 nsmbw.toml > "$out/run.log" 2>&1
echo "exit=$?" >> "$out/run.log"
cd "$T" && rm -rf -- "$w"
python3 -I "$T/test/hashes.py" "$out"
