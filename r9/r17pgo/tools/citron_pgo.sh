#!/usr/bin/env bash
# citron_scene.sh <nro> <title|map|lvl11> <secs> <outdir>
#
# One citron run (handheld, 1x, as configured in switch/citron-test/config) that reaches a fixed
# scene fast and measures it:
#   title  2 through the strap screen, then no input: title screen (the title course, a STAGE).
#          The game leaves it for the attract movie (MOVIE) after 1000 idle frames (~60 s in
#          citron): the summary reports the title windows and, per scene, the rest.
#   map    save with World 1 open (save_tpl = PC NAND template "mp"); presses 2 every 20 frames
#          through title / file select / players and stops on the World 1 map (scene WORLD_MAP).
#   lvl11  same, then 2 on the map (cursor on 1-1) and, once in the course (STAGE), the PC
#          smoke script of 1-1 (2 every 120 frames, screen-right 12 of every 120 frames).
# The scripted input needs the test NRO <nro minus .nro>_input_test.nro (build_nx.sh makes it
# next to the normal one; it is used automatically when it exists, for every scene, so the
# [SCENE] log is always there). Never ship an _input_test NRO.
#
# Machine rules: takes the machine-wide citron lock (flock switch/citron-test/.lock) and runs
# citron inside nsmbw.slice through scripts/lim (which also takes one of the 2 heavy-job
# slots); forces the "performance" power profile. The shared citron SD folder
# sdmc:/switch/NSMBWnx is backed up (all but data/) and restored at the end, even on error.
#
# Env: EVERY=n      XFB dump every n frames to <out>/shots (default 600; 0 = off)
#      EXTRA_DEBUG=f extra lines appended to debug.toml (e.g. NWII_* knobs of a track)
#      CACHE=f       shadercache.bin to install (default citron/shadercache.bin = the r8 one)
#      SAVE=dir      emulated NAND to install as save/ (default citron/save_tpl)
#      COOL=t        wait (max 3 min) until the CPU package is below t degrees C (default 65)
#      PIN=0         do not pin citron's threads (default: fixed CPUs, see sampler())
#
# Output in <outdir>: nwii.log, nwii_err.log, threads_raw.txt (per-thread CPU ticks of the
# citron process every 5 s), timeline.tsv, summary.txt (tools/citron_scene_summary.py),
# shots/*.png, citron.log, citron_log.txt, debug.toml.
set -euo pipefail
usage() { sed -n 2,3p "$0" >&2; exit 2; }
[[ $# -eq 4 ]] || usage
C=$HOME/nsmbw-port/r9/base   # r17pgo copy of r9/base/citron_scene.sh: also keeps sdmc:/switch/NSMBWnx/gcda (PGO test)
P=$HOME/nsmbw-port
NRO=$(realpath "$1"); SCENE=$2; SECS=$3; OUT=$(realpath -m "$4")
case $SCENE in title|map|lvl11) ;; *) usage ;; esac
[[ -f $NRO ]] || { echo "no such NRO: $NRO" >&2; exit 2; }
if [[ $NRO != *_input_test.nro && -f ${NRO%.nro}_input_test.nro ]]; then
  NRO=${NRO%.nro}_input_test.nro
fi
if [[ $SCENE != title && $NRO != *_input_test.nro ]]; then
  echo "scene $SCENE needs the scripted-input NRO (${NRO%.nro}_input_test.nro): build it with build_nx.sh" >&2
  exit 2
fi

# Machine-wide citron lock (one citron at a time on the whole machine).
if [[ -z ${CITRON_SCENE_LOCKED:-} ]]; then
  export CITRON_SCENE_LOCKED=1
  exec flock "$P/switch/citron-test/.lock" "$0" "$@"
fi

if command -v powerprofilesctl >/dev/null; then
  [[ $(powerprofilesctl get) == performance ]] || powerprofilesctl set performance || true
fi
SD=$HOME/.local/share/citron/sdmc/switch/NSMBWnx
[[ -d $SD/data/sys ]] || { echo "citron SD has no $SD/data/sys" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"
BK=$OUT/.sd_backup; mkdir -p "$BK"
for f in "$SD"/*; do [[ $(basename "$f") == data ]] || mv "$f" "$BK/"; done
SPID=
restore() {
  [[ -n $SPID ]] && kill "$SPID" 2>/dev/null || true
  for f in "$SD"/*; do [[ $(basename "$f") == data ]] || rm -rf "$f"; done
  for f in "$BK"/*; do [[ -e $f ]] && mv "$f" "$SD/"; done
  rmdir "$BK" 2>/dev/null || true
}
trap restore EXIT
trap 'exit 130' INT TERM

# --- SD state for this run ---
mkdir -p "$SD/cache"
cp "${CACHE:-$C/citron/shadercache.bin}" "$SD/cache/shadercache.bin"
cp -r "${SAVE:-$C/citron/save_tpl}" "$SD/save"
EVERY=${EVERY:-600}
{
  echo "# citron_scene.sh $SCENE (r9/base): solo pruebas"
  echo "fps_log = true"
  if [[ $EVERY != 0 ]]; then
    echo "NWII_XFBDUMP = \"sdmc:/switch/NSMBWnx/shots/t\""
    echo "NWII_XFBDUMP_EVERY = $EVERY"
  fi
  case $SCENE in
    title)  # 2 through the strap screen only; nothing once the title course starts loading
      echo 'NWII_INPUT_SCRIPT = "0-100000:100:20:4"'
      echo 'NWII_INPUT_PHASES = "6"' ;;
    map)
      echo 'NWII_INPUT_SCRIPT = "0-100000:100:20:4"'
      echo 'NWII_INPUT_PHASES = "3"' ;;
    lvl11)
      echo 'NWII_INPUT_SCRIPT = "0-100000:100:20:4"'
      echo 'NWII_INPUT_PHASES = "3>5"'
      echo 'NWII_INPUT_SCRIPT_1 = "0-100000:100:20:4"'
      echo 'NWII_INPUT_SCRIPT_2 = "0-10000000:100:120:4;60-10000000:4:120:12"' ;;
  esac
  [[ -n ${EXTRA_DEBUG:-} ]] && cat "$EXTRA_DEBUG"
} > "$SD/debug.toml"
cp "$SD/debug.toml" "$OUT/debug.toml"
# Same thermal start for every run: the laptop sits at 90-96 C under citron and the P-cores
# drop to ~3 GHz, so wait (max 3 min) until the package is below COOL (default 65 C).
TF=; for h in /sys/class/hwmon/hwmon*; do [[ $(cat "$h/name" 2>/dev/null) == coretemp ]] && TF=$h/temp1_input; done
if [[ -n $TF ]]; then
  for ((i = 0; i < 90; i++)); do (( $(cat "$TF") < ${COOL:-65} * 1000 )) && break; sleep 2; done
fi
echo "nro=$NRO scene=$SCENE secs=$SECS pin=${PIN:-1} power=$(powerprofilesctl get 2>/dev/null) start=$(date +%s) temp_start=$(( $(cat "${TF:-/dev/null}" 2>/dev/null || echo 0) / 1000 ))C" > "$OUT/run_info.txt"
sha256sum "$NRO" >> "$OUT/run_info.txt"

# --- per-thread CPU sampler (citron process, every 5 s) ---
sampler() {
  local pid= t0 log=$SD/logs/nwii.log tf=/dev/null h
  declare -A PINNED=()
  for h in /sys/class/hwmon/hwmon*; do [[ $(cat "$h/name" 2>/dev/null) == coretemp ]] && tf=$h/temp1_input; done
  local NCPU; NCPU=$(nproc --all)
  t0=$(date +%s.%N)
  while :; do
    if [[ -z $pid || ! -d /proc/$pid ]]; then
      pid=
      for p in $(pgrep -u "$(id -u)" -f -- "-g $NRO" || true); do
        [[ $(cat /proc/$p/comm 2>/dev/null) == citron ]] && pid=$p
      done
    fi
    # Rule: always "performance". Something switched it to power-saver once in the middle of a
    # run (31-oct, title_r8_b: P-cores at ~1.4 GHz): put it back and leave a W line.
    local pp; pp=$(powerprofilesctl get 2>/dev/null || echo unknown)
    if [[ $pp != performance && $pp != unknown ]]; then
      echo "W $(date +%s) power profile was $pp: set back to performance" >> "$OUT/threads_raw.txt"
      powerprofilesctl set performance || true
    fi
    if [[ -n $pid && ${PIN:-1} != 0 ]]; then
      # Fixed placement (PIN=1, default): runs differed up to 2x in fps when the game thread
      # shared a P-core with another busy thread (HT sibling) or moved to an E-core.
      #   CPUCore_1 (game CPU thread) -> CPU 0, CPUCore_0 (main: render + GL) -> CPU 1
      #   (their HT siblings 5 and 2 stay free of citron), CPUCore_2/3 -> 3,4,
      #   GPU/Vulkan -> 6,7, everything else (audio emulation, services) -> E-cores 8-15.
      for t in /proc/$pid/task/*; do
        local tid=${t##*/} comm cpus
        [[ -n ${PINNED[$tid]:-} ]] && continue
        comm=$(cat "$t/comm" 2>/dev/null) || continue
        case $comm in
          CPUCore_1) cpus=0 ;; CPUCore_0) cpus=1 ;; CPUCore_2|CPUCore_3) cpus=3,4 ;;
          GPU|VulkanWorker|GPUFencingThrea) cpus=6,7 ;;
          *) cpus=8-15 ;;
        esac
        taskset -p -c "$cpus" "$tid" > /dev/null 2>&1 && PINNED[$tid]=$cpus
      done
    fi
    if [[ -n $pid ]]; then
      local now nx sc
      now=$(date +%s.%N)
      nx=$(grep -c '^\[NX\] game' "$log" 2>/dev/null || true)
      sc=$(grep -c '^\[SCENE\]' "$log" 2>/dev/null || true)
      {
        echo "T $(awk -v a="$now" -v b="$t0" 'BEGIN{printf "%.2f", a-b}') pid=$pid nx=${nx:-0} scene_lines=${sc:-0}"
        # host: package temperature (m°C) and current MHz of every CPU (0-7 P, 8-15 E, 16-17 LP-E)
        echo "H temp=$(cat "$tf" 2>/dev/null) mhz=$(for ((i = 0; i < NCPU; i++)); do cat /sys/devices/system/cpu/cpu$i/cpufreq/scaling_cur_freq 2>/dev/null || echo 0; done | awk '{printf "%s%d", (NR>1?",":""), $1/1000}')"
        # tid comm utime+stime(ticks) last_cpu
        for t in /proc/$pid/task/*; do
          s=$(cat "$t/stat" 2>/dev/null) || continue
          echo "$(basename "$t") $(tr ' ' _ < "$t/comm") $(echo "${s##*) }" | awk '{print $12+$13, $37}')"
        done
      } >> "$OUT/threads_raw.txt"
    fi
    sleep 5
  done
}
sampler &
SPID=$!

# --- run (citron_test.sh: headless KWin, audio null; inside nsmbw.slice via lim) ---
"$P/scripts/lim" "$P/switch/citron_test.sh" "$NRO" "$SECS" "$OUT/citron" || true
kill "$SPID" 2>/dev/null || true; wait "$SPID" 2>/dev/null || true; SPID=

cp "$SD/logs/nwii.log" "$OUT/" 2>/dev/null || true
[[ -d $SD/gcda ]] && cp -r "$SD/gcda" "$OUT/gcda"
cp "$SD/logs/nwii_err.log" "$OUT/" 2>/dev/null || true
cp "$SD/config.toml" "$OUT/sd_config.toml" 2>/dev/null || true
mv "$OUT/citron/citron.log" "$OUT/citron/citron_log.txt" "$OUT/" 2>/dev/null || true
if [[ -d $SD/shots ]]; then
  mkdir -p "$OUT/shots"
  for f in "$SD"/shots/*.ppm; do
    [[ -e $f ]] || continue
    magick "$f" -resize 50% "$OUT/shots/$(basename "${f%.ppm}").png" 2>/dev/null || true
  done
fi
restore; trap - EXIT
rm -rf "$OUT/citron/session.sh"
python3 -I "$C/tools/citron_scene_summary.py" "$OUT" "$SCENE" > "$OUT/summary.txt" || true
cat "$OUT/summary.txt"
