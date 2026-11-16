#!/usr/bin/env bash
# df_ab.sh <bin_input_test> <demo> <sleep_us> [secs=100]: NWII_V_DROPFAST apagado/encendido con el trabajador frenado
# (NWII_V_SLOWWORKER_US), FRAMECHECK=10 + comprobacion de coalesce a 1,5789x.
W=/home/djvemo/nsmbw-port/r9/r37v; cd $W; bin=$1; demo=$2; us=$3; secs=${4:-100}
for v in off on; do ex=(); [[ $v == on ]] && ex=(NWII_V_DROPFAST=1); tag=df_${demo}_${us}_$v
  flock pc/.runlock env NWII_FORCE_DEMO=$demo NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 NWII_R9_FRAMECHECK=10 NWII_R_COALESCE_CHECK=1 \
    NWII_V_SLOWWORKER_US=$us "${ex[@]}" EVERY=0 PROFILE=free RES=1.5789 pc/run.sh "$bin" $tag $secs mp "0-$secs:100:20:4" > /dev/null 2>&1
  L=pc/runs/$tag/run.log
  echo "== $tag: $(grep -a FRAMECHECK $L | tail -1 | sed 's/.*checks=/checks=/' | cut -c1-48) diff_lines=$(grep -a FRAMECHECK $L | grep -vc ' 0 pixels differ') | $(grep -a RCOAL $L | tail -1 | grep -o 'check: .*') | baratos=$(grep -a '\[V\] descartes' $L | awk '{s+=$4} END{print s+0}') | $(tail -1 $L)"
  grep -a "^\[WPROF\]" $L | tail -8 | awk '{for(i=1;i<=NF;i++){if($i~/^(frames|busy_us\/frame|parse|split|decode)=/)printf "%s ",$i}; print ""}' | tail -4
  grep -a "^\[RPROF\]" $L | tail -3 | grep -o "frames=[0-9]*\|dropped_frames=[0-9]*\|retracts=[0-9]*" | paste -sd' '
done
