#!/usr/bin/env bash
# checks.sh <binary_input_test> <tag> [secs=120]
#   PC correctness runs of r9:render (offscreen, silent, private NAND mp): every 30th frame drawn twice
#   (R9 path, then r8 path) and compared pixel by pixel (NWII_R9_FRAMECHECK), every emit-cache hit
#   recomputed and compared (NWII_ECACHE_CHECK); title, map (idle and walking) and 1-1, at 1x and 2x.
set -u
T=/home/djvemo/nsmbw-port/r9/verif_omen/r41m
bin=$1; tag=$2; secs=${3:-120}
export NWII_R9_FRAMECHECK=30 NWII_ECACHE_CHECK=1
for res in ${RESS:-1 2}; do
for sc in title map walk lvl11; do
  export NWII_INPUT_SCRIPT_1= NWII_INPUT_SCRIPT_2=
  case $sc in
    title) export NWII_INPUT_PHASES=6 ;;
    map)   export NWII_INPUT_PHASES=3 ;;
    walk)  export NWII_INPUT_PHASES=3 NWII_INPUT_SCRIPT_1='60-100000:4:300:30;210-100000:8:300:30' ;;
    lvl11) export NWII_INPUT_PHASES='3>5' NWII_INPUT_SCRIPT_1='0-100000:100:20:4' \
                  NWII_INPUT_SCRIPT_2='0-10000000:100:120:4;60-10000000:4:120:12' ;;
  esac
  name=chk_${tag}_${sc}_${res}x
  EVERY=900 PROFILE=free RES=$res "$T/pc/run.sh" "$bin" "$name" "$secs" mp '0-100000:100:20:4' > /dev/null 2>&1
  L=$T/pc/runs/$name/run.log
  printf '%-22s %s | %s | scenes: %s\n' "$name" \
    "$(grep FRAMECHECK "$L" | tail -1 | sed 's/.*checks=/checks=/; s/ | xfb.*//') diff_lines=$(grep FRAMECHECK "$L" | grep -vc ' 0 pixels differ')" \
    "$(grep '^\[ECACHE' "$L" | awk '{for(i=1;i<=NF;i++){if($i~/^checked=/)c=$i; if($i~/^mismatches=/)m=$i}} END{print c, m}')" \
    "$(grep -o 'scene=[0-9]* ([A-Z_]*)' "$L" | awk '{print $2}' | uniq | tr '\n' ' ')"
  printf '%-22s %s | %s | %s\n' "" "$(grep -a D40CHECK "$L" | tail -1)" "$(grep -a 'RCOAL' "$L" | tail -1 | grep -o 'check: .*')" "$(tail -1 "$L")"
done
done
