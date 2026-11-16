#!/usr/bin/env bash
# ab_g5.sh <scene: title|map|lvl11|cb4> [secs=120] [order=ABBA] : A/B r13 vs r14g5 (input_test binaries)
# with a weak-CPU profile (PROFILE, default e2: CPUs 14,15, 150% quota) in the private run folder
# r14g5/pc_ab (no shared state, no lock needed). Prints fps, cpu thread %, guest Minst/s averaged over
# the [FPS] lines after the first 40 s, and the scenes seen.
#   title: 2 until past the strap screen, then idle: title attract demos (4-player demo levels) and
#          Peach's party (MOVIE) at ~1000 idle frames.   map: World 1 map idle.   lvl11: 1-1 smoke.
#   cb4: Coin Battle 4 players (script of r9/r13/tools/verify_r13.sh).
set -u
R=/home/djvemo/nsmbw-port/r9
A=${A:-$R/r13/bin/NSMBW_r13_input_test}
B=${B:-$R/r14g5/bin/NSMBW_r14g5_input_test}
PP=$R/r14g5/pc_ab
S11='0-10000000:100:120:4;60-10000000:4:120:12'
CB4='0-610:100:120:4;700-900:1:50:4;920-1000:4:40:4;1040-1100:100:60:4;1500-1600:4:60:4;1650-1660:100:10:4;1950-1960:10:10:4;2500-2510:10:10:4;3100-100000:100:150:4;3800-100000:4:100:60;p2:on@2100;p2:2700-2900:100:100:4;p2:3800-100000:8:150:80;p3:on@2200;p3:2730-2900:100:100:4;p3:3800-100000:100:90:6;p4:on@2300;p4:2760-2900:100:100:4;p4:3800-100000:10000:120:10;p4:3800-100000:4:300:100'
sc=$1; secs=${2:-120}; order=${3:-ABBA}
fpsline() { grep '^\[FPS' "$1" | tail -n +9 | awk '{f+=$2; n++; for(i=1;i<=NF;i++){ if($i=="thread" && $(i-1)=="cpu"){c+=$(i+1)} if($i=="Minst/s,"){m+=$(i-1)} }} END{if(n) printf "fps=%.2f cpu_thread=%.1f%% guest=%.1fMinst/s n=%d", f/n, c/n, m/n, n}'; }
i=0
for (( k=0; k<${#order}; k++ )); do
  b=${order:$k:1}; i=$((i + 1)); bin=$A; [[ $b == B ]] && bin=$B
  unset NWII_INPUT_PHASES NWII_INPUT_SCRIPT_1 NWII_INPUT_SCRIPT_2
  toml=default.toml; script='0-100000:100:20:4'
  case $sc in
    title) script='0-330:100:20:4' ;;
    map)   export NWII_INPUT_PHASES=3 ;;
    lvl11) export NWII_INPUT_PHASES='3>5' NWII_INPUT_SCRIPT_1='0-100000:100:20:4' NWII_INPUT_SCRIPT_2="$S11" ;;
    cb4)   toml=mp.toml; script=$CB4 ;;
  esac
  n=g5_ab_${sc}_${i}_$b
  env TOML=$toml EVERY=0 PROFILE=${PROFILE:-e2} RES=1 $PP/run.sh $bin $n $secs mp "$script" > /dev/null 2>&1
  L=$PP/runs/$n/run.log
  echo "ab $sc $i $b: $(fpsline $L) | $(grep -o 'scene=[0-9]* ([A-Z_]*)' $L | awk '{print $2}' | uniq | tr '\n' ' ') | $(tail -1 $PP/runs/$n/summary.txt 2>/dev/null | head -c 0)$(grep -c 'FASTMEM\] site' $L) fmsites $(grep -m1 -c -i 'segmentation\|abort' $L) crash"
done
