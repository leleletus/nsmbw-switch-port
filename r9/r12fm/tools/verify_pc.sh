#!/usr/bin/env bash
# verify_pc.sh <phase: smoke|det|ab> : PC verification of the R12 fastmem binary against r10c.
#  smoke: scripted runs (title/demos, menus->map->1-1, map walking, Coin Battle 4P) of the fastmem
#         binary with r9/render/pc/run.sh (shared folder -> flock .runlock); scene sequence, fps,
#         [FASTMEM] fault dump per run -> r12fm/runs/smoke_<sc>.txt
#  det:   deterministic runs (r9/render/test/det.sh: virtual clock, no frame skip, every 5th XFB copy
#         hashed) of r10c and r12fm on title, map, 1-1: frame hashes must be identical.
#  ab:    A/B with the Switch-like CPU profile (limc sw), ABBA, map and 1-1 (r9/render/test/pc_ab.sh
#         style: fps, cpu thread %, guest Minst/s).
set -u
R=/home/djvemo/nsmbw-port/r9
FM=$R/r12fm/bin/NSMBW_r12fm_input_test
BASE=$R/r10c/bin/NSMBW_r10c_input_test
PC=$R/render/pc
PP=$R/r12fm/pc   # private copy of the run folder (own run/, runs/): no shared state, pinned with q0/q1 (limc)
OUT=$R/r12fm/runs
LOCK=$PC/.runlock
S11='0-10000000:100:120:4;60-10000000:4:120:12'
CB4='0-610:100:120:4;700-900:1:50:4;920-1000:4:40:4;1040-1100:100:60:4;1500-1600:4:60:4;1650-1660:100:10:4;1950-1960:10:10:4;2500-2510:10:10:4;3100-100000:100:150:4;3800-100000:4:100:60;p2:on@2100;p2:2700-2900:100:100:4;p2:3800-100000:8:150:80;p3:on@2200;p3:2730-2900:100:100:4;p3:3800-100000:100:90:6;p4:on@2300;p4:2760-2900:100:100:4;p4:3800-100000:10000:120:10;p4:3800-100000:4:300:100'
scenes() { grep -o 'scene=[0-9]* ([A-Z_]*)' "$1" | awk '{print $2}' | uniq | tr '\n' ' '; }
fpsline() { grep '^\[FPS' "$1" | awk '{f+=$2; n++; for(i=1;i<=NF;i++){ if($i=="thread" && $(i-1)=="cpu"){c+=$(i+1)} if($i=="Minst/s,"){m+=$(i-1)} }} END{if(n) printf "fps=%.1f cpu_thread=%.0f%% guest=%.0fMinst/s n=%d", f/n, c/n, m/n, n}'; }
case $1 in
smoke)
  for sc in ${SCEN:-idle lvl11 walk cb4}; do
    unset NWII_INPUT_PHASES NWII_INPUT_SCRIPT_1 NWII_INPUT_SCRIPT_2
    n=r12fm_smoke_$sc
    case $sc in
      idle)  env PROFILE=q2 EVERY=900 NWII_FASTMEM_DUMP_S=30 $PP/run.sh $FM $n ${T_IDLE:-180} fresh ;;
      lvl11) env PROFILE=q2 EVERY=900 NWII_FASTMEM_DUMP_S=30 $PP/run.sh $FM $n ${T_LVL:-240} mp "$S11" ;;
      walk)  env PROFILE=q2 NWII_INPUT_PHASES=3 NWII_INPUT_SCRIPT_1='60-100000:4:300:30;210-100000:8:300:30' \
               EVERY=900 NWII_FASTMEM_DUMP_S=30 $PP/run.sh $FM $n ${T_WALK:-120} mp '0-100000:100:20:4' ;;
      cb4)   env PROFILE=q2 TOML=mp.toml EVERY=600 NWII_FASTMEM_DUMP_S=30 $PP/run.sh $FM $n ${T_CB4:-200} mp "$CB4" ;;
    esac > /dev/null 2>&1
    L=$PP/runs/$n/run.log
    { echo "== $sc $(date +%T)"; echo "scenes: $(scenes $L)"; echo "$(fpsline $L)"; tail -1 $L
      grep -m3 -i 'unhandled\|segmentation\|abort\|crash' $L
      awk '/^\[FASTMEM\] dump/{last=NR} {l[NR]=$0} END{for(i=last;i<=NR;i++) if(l[i]~/^\[FASTMEM\]/) print l[i]}' $L | head -40
      cat $PP/runs/$n/summary.txt; } > $OUT/smoke_$sc.txt 2>&1
    cat $OUT/smoke_$sc.txt | head -12
  done ;;
det)
  for sc in ${SCEN:-title map lvl11}; do
    PROFILE=q0 $R/r12fm/tools/det.sh $BASE r12fm_det_${sc}_base $sc ${T_DET:-80} 5 > /dev/null 2>&1 &
    PROFILE=q1 $R/r12fm/tools/det.sh $FM r12fm_det_${sc}_fm $sc ${T_DET:-80} 5 > /dev/null 2>&1 &
    wait
    for b in base fm; do python3 -I $R/render/test/hashes.py $R/r12fm/test/runs/r12fm_det_${sc}_$b > /dev/null; done
    D=$R/r12fm/test/runs
    a=$D/r12fm_det_${sc}_base/hashes.txt; b=$D/r12fm_det_${sc}_fm/hashes.txt
    na=$(wc -l < $a); nb=$(wc -l < $b); m=$(( na < nb ? na : nb ))
    d=$(paste -d' ' <(head -$m $a) <(head -$m $b) | awk '$3!=$6' | wc -l)
    first=$(paste -d' ' <(head -$m $a) <(head -$m $b) | awk '$3!=$6{print $1; exit}')
    echo "det $sc: base $na frames, fm $nb frames, compared $m, differing $d ${first:+(first $first)}; scenes base: $(scenes $D/r12fm_det_${sc}_base/run.log) | fm: $(scenes $D/r12fm_det_${sc}_fm/run.log)" | tee -a $OUT/det.txt
  done ;;
det2)
  # det.sh with the AX mixer inline (NWII_AX_THREAD=0): base twice (is it deterministic?) and fm
  sc=${SC:-lvl11}; D=$R/r12fm/test/runs
  EXTRA="NWII_AX_THREAD=0" PROFILE=q0 $R/r12fm/tools/det.sh $BASE r12fm_det2_${sc}_base $sc ${T_DET:-100} 5 > /dev/null 2>&1 &
  EXTRA="NWII_AX_THREAD=0" PROFILE=q3 $R/r12fm/tools/det.sh $BASE r12fm_det2_${sc}_base2 $sc ${T_DET:-100} 5 > /dev/null 2>&1 &
  EXTRA="NWII_AX_THREAD=0" PROFILE=q1 $R/r12fm/tools/det.sh $FM r12fm_det2_${sc}_fm $sc ${T_DET:-100} 5 > /dev/null 2>&1 &
  wait
  for b in base base2 fm; do python3 -I $R/render/test/hashes.py $D/r12fm_det2_${sc}_$b > /dev/null; done
  cmp2() { local a=$D/r12fm_det2_${sc}_$1/hashes.txt b=$D/r12fm_det2_${sc}_$2/hashes.txt
    local na=$(wc -l < $a) nb=$(wc -l < $b); local m=$(( na < nb ? na : nb ))
    echo "det2 $sc $1 vs $2: $na/$nb frames, compared $m, differing $(paste -d' ' <(head -$m $a) <(head -$m $b) | awk '$3!=$6' | wc -l) first $(paste -d' ' <(head -$m $a) <(head -$m $b) | awk '$3!=$6{print $1; exit}')"; }
  { cmp2 base base2; cmp2 base fm; cmp2 base2 fm; } | tee -a $OUT/det.txt ;;
ab)
  for sc in ${SCEN:-lvl11 map}; do
    i=0
    for b in base fm fm base; do
      i=$((i + 1)); bin=$BASE; [[ $b == fm ]] && bin=$FM
      unset NWII_INPUT_PHASES NWII_INPUT_SCRIPT_1 NWII_INPUT_SCRIPT_2
      case $sc in
        map)   export NWII_INPUT_PHASES=3 ;;
        lvl11) export NWII_INPUT_PHASES='3>5' NWII_INPUT_SCRIPT_1='0-100000:100:20:4' NWII_INPUT_SCRIPT_2="$S11" ;;
      esac
      n=r12fm_ab_${sc}_${i}_$b
      flock $LOCK env EVERY=0 PROFILE=sw RES=1 $PC/run.sh $bin $n ${T_AB:-100} mp '0-100000:100:20:4' > /dev/null 2>&1
      L=$PC/runs/$n/run.log
      # skip the first 40 s of [FPS] lines (boot, menus)
      echo "ab $sc $i $b: $(awk 'NR>0' $L | grep '^\[FPS' | tail -n +9 > /tmp/claude-1000/fps_$$.txt; fpsline /tmp/claude-1000/fps_$$.txt) scenes: $(scenes $L)" | tee -a $OUT/ab.txt
    done
  done ;;
esac
