#!/usr/bin/env bash
# run.sh <binary> <name> <secs> <nand: mp|fresh|glitch> [input_script]
#   (r9/base/pc: copy of verify/integration_r8/run.sh; NWII_INPUT_PHASES / NWII_INPUT_SCRIPT_<k> pass through the environment)
#   env: TOML (default.toml|mp.toml), EVERY (dump every n frames, 0=off), PROFILE (limc CPU profile, default free)
#        RES (NWII_INTERNAL_RES; empty = toml/auto), SCALED=1 (NWII_DUMP_SCALED), GPUTIME=1 (NWII_GPUTIME)
# Offscreen + silent run inside nsmbw.slice (memory cap) with a private NAND copy and warm shader cache.
set -u
T=$(cd "$(dirname "$0")" && pwd)   # this folder (works in a copy of r9/base)
bin=$(realpath "$1"); name=$2; secs=$3; tpl=$4; script=${5:-}
[[ -n $name && -n $tpl ]] || exit 2
out=$T/runs/$name
rm -rf -- "$out"; mkdir -p "$out" $T/run
cd $T/run || exit 1
rm -rf -- ./nand; cp -a "$T/nand_tpl_$tpl" ./nand
rm -f ./shadercache.bin; [[ -f $T/shadercache.tpl && -z "${COLD:-}" ]] && cp $T/shadercache.tpl ./shadercache.bin
cp "$T/${TOML:-default.toml}" ./nsmbw.toml
sed -i "s#^nand_dir = .*#nand_dir = \"$T/run/nand\"#; s#^output_dir = .*#output_dir = \"$T/unused_export\"#" ./nsmbw.toml
ev=(); [[ ${EVERY:-240} != 0 ]] && ev=(NWII_XFBDUMP=$out/t NWII_XFBDUMP_EVERY=${EVERY:-240})
sc=(); [[ -n $script ]] && sc=(NWII_INPUT_SCRIPT="$script" NWII_INPUT_LOG=1 NWII_WPAD_LOG=1)
ex=(); [[ -n ${RES:-} ]] && ex+=(NWII_INTERNAL_RES=$RES); [[ -n ${SCALED:-} ]] && ex+=(NWII_DUMP_SCALED=1); [[ -n ${GPUTIME:-} ]] && ex+=(NWII_GPUTIME=1)
env SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy NWII_FPS=1 "${ev[@]}" "${sc[@]}" "${ex[@]}" \
  PROFILE=${PROFILE:-free} $T/limc timeout "$secs" stdbuf -oL -eL "$bin" /home/djvemo/nsmbw-port/game/SMNE01 nsmbw.toml > "$out/run.log" 2>&1
echo "exit=$? profile=${PROFILE:-free} power=$(powerprofilesctl get 2>/dev/null)" >> "$out/run.log"
cd "$out" || exit 1
for f in t_*.ppm; do [ -e "$f" ] && magick "$f" -resize "${RESIZE:-50%}" "${f%.ppm}.png" && rm -f "$f"; done
python3 -I /home/djvemo/nsmbw-port/cpu/tools/hang_summary.py run.log > summary.txt
grep '^\[FPS' run.log | awk '{s+=$2; n++; if(min==""||$2<min)min=$2} END{printf "fps avg=%.1f min=%.1f n=%d\n", s/n, min, n}' >> summary.txt
echo "gov_lines=$(grep -c '^\[Gov' run.log) res_line=$(grep -m1 'internal resolution' run.log)" >> summary.txt
grep -m1 GPUTIME run.log >> summary.txt
echo "resume_pruned_lines=$(grep -c "resume_pruned" run.log) shader_new=$(grep -o "shaders: new=[0-9]*" run.log | awk -F= "{s+=\$2} END{print s+0}")" >> summary.txt
cat summary.txt; du -sh .
