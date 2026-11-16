#!/usr/bin/env bash
# ab_private.sh <scene: lvl11|map> [secs=100] : ABBA r10c vs r12fm with a weak-CPU profile in the private
# run folder r12fm/pc (no shared state). Profile PROFILE (default e2: CPUs 14,15 @3.6 GHz, 150% quota; the
# shared "sw" pair 16,17 was busy with other agents' locked A/B). Prints fps, cpu thread %, guest Minst/s
# averaged over the [FPS] lines after the first 40 s.
set -u
R=/home/djvemo/nsmbw-port/r9
FM=$R/r12fm/bin/NSMBW_r12fm_input_test
BASE=$R/r10c/bin/NSMBW_r10c_input_test
PP=$R/r12fm/pc_ab
S11='0-10000000:100:120:4;60-10000000:4:120:12'
sc=$1; secs=${2:-100}
fpsline() { grep '^\[FPS' "$1" | tail -n +9 | awk '{f+=$2; n++; for(i=1;i<=NF;i++){ if($i=="thread" && $(i-1)=="cpu"){c+=$(i+1)} if($i=="Minst/s,"){m+=$(i-1)} if($i=="thread" && $(i-1)=="host"){h+=$(i+1)} }} END{if(n) printf "fps=%.2f cpu_thread=%.1f%% host_thread=%.1f%% guest=%.1fMinst/s n=%d", f/n, c/n, h/n, m/n, n}'; }
i=0
for b in base fm fm base; do
  i=$((i + 1)); bin=$BASE; [[ $b == fm ]] && bin=$FM
  unset NWII_INPUT_PHASES NWII_INPUT_SCRIPT_1 NWII_INPUT_SCRIPT_2
  case $sc in
    map)   export NWII_INPUT_PHASES=3 ;;
    lvl11) export NWII_INPUT_PHASES='3>5' NWII_INPUT_SCRIPT_1='0-100000:100:20:4' NWII_INPUT_SCRIPT_2="$S11" ;;
  esac
  n=r12fm_ab_${sc}_${i}_$b
  env EVERY=0 PROFILE=${PROFILE:-e2} RES=1 $PP/run.sh $bin $n $secs mp '0-100000:100:20:4' > /dev/null 2>&1
  L=$PP/runs/$n/run.log
  echo "ab $sc $i $b: $(fpsline $L) | $(grep -o 'scene=[0-9]* ([A-Z_]*)' $L | awk '{print $2}' | uniq | tr '\n' ' ')"
done
