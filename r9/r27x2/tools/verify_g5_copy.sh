#!/usr/bin/env bash
# verify_g5.sh (copy of r13/tools/verify_r13.sh, r14g5 binary) <scenario...> : PC runs of the r13 input_test binary (fastmem + render worker + GPU XF)
# with the shared harness r9/render/pc/run.sh, each run under flock r9/render/pc/.runlock.
#   sanity : title -> menus -> map -> 1-1, 165 s (script S11 of r12fm smoke lvl11)
#   long   : the same, 300 s
#   cb4    : Coin Battle 4 players, 200 s (script CB4 of r12fm verify_pc.sh)
#   iris   : title iris frames (r9/r11d/LEEME.md: phases 6, dump every 2 frames from 330)
#   title  : title left idle: attract demos (4-player demo levels) + Peach's party (MOVIE), 200 s
#   off    : sanity with NWII_RENDER_WORKER=0 NWII_GPU_XF=0 (both switches off), 120 s
# Summary per run -> r13/runs/<scenario>.txt
set -u
R=/home/djvemo/nsmbw-port/r9
BIN=${BIN:-$R/r14g5/bin/NSMBW_r14g5_input_test}
PC=${PC:-$R/render/pc}
LOCK=${LOCK:-$PC/.runlock}
OUT=${OUT:-$R/r14g5/runs}; mkdir -p "$OUT"
S11='0-10000000:100:120:4;60-10000000:4:120:12'
CB4='0-610:100:120:4;700-900:1:50:4;920-1000:4:40:4;1040-1100:100:60:4;1500-1600:4:60:4;1650-1660:100:10:4;1950-1960:10:10:4;2500-2510:10:10:4;3100-100000:100:150:4;3800-100000:4:100:60;p2:on@2100;p2:2700-2900:100:100:4;p2:3800-100000:8:150:80;p3:on@2200;p3:2730-2900:100:100:4;p3:3800-100000:100:90:6;p4:on@2300;p4:2760-2900:100:100:4;p4:3800-100000:10000:120:10;p4:3800-100000:4:300:100'
scenes() { grep -o 'scene=[0-9]* ([A-Z_]*)' "$1" | awk '{print $2}' | uniq | tr '\n' ' '; }
fpsline() { grep '^\[FPS' "$1" | awk '{f+=$2; n++; for(i=1;i<=NF;i++){ if($i=="thread" && $(i-1)=="cpu"){c+=$(i+1)} if($i=="worker" && $(i-1)=="gx"){w+=$(i+1)} if($i=="Minst/s,"){m+=$(i-1)} }} END{if(n) printf "fps=%.1f cpu_thread=%.0f%% gx_worker=%.0f%% guest=%.0fMinst/s n=%d", f/n, c/n, w/n, m/n, n}'; }
report() {  # report <run name> <scenario>
  local L=$PC/runs/$1/run.log
  { echo "== $2 $(date '+%F %T') bin=$BIN"
    echo "scenes: $(scenes $L)"
    echo "$(fpsline $L)"
    tail -1 $L
    grep -m3 -i 'segmentation\|abort\|crash\|fatal' $L
    grep -m1 '^\[GX\] render worker' $L
    grep '^\[RPROF\]' $L | grep -o 'worker=[0-9]* retracts=[0-9]*' | sort | uniq -c | sort -rn | head -3
    grep -m3 '^\[GPUXF\] \(self-test\|on\|off\)' $L
    grep '^\[GPUXF\] per frame' $L | tail -1
    awk '/^\[FASTMEM\] dump/{last=NR} {l[NR]=$0} END{if(last) for(i=last;i<=NR;i++) if(l[i]~/^\[FASTMEM\]/) print l[i]}' $L | head -20
    cat $PC/runs/$1/summary.txt; } > $OUT/$2.txt 2>&1
  cat $OUT/$2.txt
}
for sc in "$@"; do
  unset NWII_INPUT_PHASES NWII_INPUT_SCRIPT_1 NWII_INPUT_SCRIPT_2
  n=g5_${TAG:-r14g5}_$sc
  case $sc in
    sanity) flock $LOCK env EVERY=900 NWII_FASTMEM_DUMP_S=30 $PC/run.sh $BIN $n ${T_SAN:-165} mp "$S11" ;;
    long)   flock $LOCK env EVERY=1800 NWII_FASTMEM_DUMP_S=60 $PC/run.sh $BIN $n ${T_LONG:-300} mp "$S11" ;;
    cb4)    flock $LOCK env TOML=mp.toml EVERY=600 NWII_FASTMEM_DUMP_S=30 $PC/run.sh $BIN $n ${T_CB4:-200} mp "$CB4" ;;
    title)  flock $LOCK env EVERY=900 NWII_FASTMEM_DUMP_S=30 $PC/run.sh $BIN $n ${T_TITLE:-200} mp '0-330:100:20:4' ;;
    off)    flock $LOCK env NWII_RENDER_WORKER=0 NWII_GPU_XF=0 EVERY=900 NWII_FASTMEM_DUMP_S=30 \
              $PC/run.sh $BIN $n ${T_OFF:-120} mp "$S11" ;;
    iris)   for w in 1 0; do
              flock $LOCK env NWII_RENDER_WORKER=$w NWII_INPUT_PHASES=6 NWII_INPUT_SCRIPT_1= NWII_XFBDUMP_FROM=330 \
                EVERY=2 RESIZE=40% PROFILE=free RES=1 $PC/run.sh $BIN ${n}_w$w 16 mp '0-330:100:20:4'
              rm -rf $OUT/iris_w$w; cp -a $PC/runs/${n}_w$w $OUT/iris_w$w
            done ;;
  esac > /dev/null 2>&1
  [[ $sc == iris ]] || report $n $sc
done
