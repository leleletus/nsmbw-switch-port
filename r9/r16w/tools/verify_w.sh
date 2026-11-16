#!/usr/bin/env bash
# verify_w.sh <name> [steps...]   PC verification of r16w binary bin/NSMBW_<name>_input_test.
#   steps: checks (render/test/checks.sh 1x, FRAMECHECK), wcheck (NWII_WGP_CHECK stream check, title 100 s),
#          off (NWII_RENDER_WORKER=0, title+demos 150 s), long (title 240 s then map 180 s)
#   Every game run takes r9/render/pc/.runlock. Results: r16w/runs/<step>_<name>.txt
set -u
W=/home/djvemo/nsmbw-port/r9/r16w; R=/home/djvemo/nsmbw-port/r9/render
n=$1; shift
bin=$W/bin/NSMBW_${n}_input_test
lock() { flock "$R/pc/.runlock" "$@"; }
sum() {  # <run name>: scenes, fps, WGP/IDXSNAP/FASTMEM/GPUXF/worker lines
  local L=$R/pc/runs/$1/run.log
  echo "== $1"; head -3 "$R/pc/runs/$1/summary.txt"
  echo "scenes: $(grep -o 'scene=[0-9]* ([A-Z_]*)' "$L" | awk '{print $2}' | uniq | tr '\n' ' ')"
  grep -m1 '\[WGP\] write-gather' "$L"; grep -m1 'render worker:' "$L"; grep -m1 'GPUXF\] self-test' "$L"
  grep '\[FASTMEM\]' "$L" | tail -1; grep '\[IDXSNAP\]' "$L" | tail -1; grep '\[WGP\] check' "$L" | tail -3
  grep '^\[FPS' "$L" | tail -2 | sed 's/.*\(wgp [^,]*\).*/\1/'
}
for st in "$@"; do
  case $st in
    checks)
      (cd $R && lock env RESS=1 test/checks.sh "$bin" w16_$n) > $W/runs/checks_$n.txt 2>&1 ;;
    wcheck)
      rm -f $W/runs/wcheck_$n.*
      (cd $R && lock env NWII_INPUT_PHASES=6 NWII_WGP_CHECK=$W/runs/wcheck_$n EVERY=0 PROFILE=free RES=1 \
         pc/run.sh "$bin" w16_wcheck_$n 100 mp '0-100000:100:20:4') > /dev/null 2>&1
      { sum w16_wcheck_$n
        w=$W/runs/wcheck_$n.w f=$W/runs/wcheck_$n.f
        sw=$(stat -c %s $w) sf=$(stat -c %s $f)
        echo "stream: written $sw B, handed over $sf B, prefix equal: $(cmp -n $sf $w $f > /dev/null && echo yes || echo NO)"
      } > $W/runs/wcheck_$n.txt 2>&1
      rm -f $W/runs/wcheck_$n.w $W/runs/wcheck_$n.f ;;
    off)
      (cd $R && lock env NWII_INPUT_PHASES=6 NWII_RENDER_WORKER=0 NWII_IDXSNAP_LOG=1 EVERY=600 PROFILE=free RES=1 \
         pc/run.sh "$bin" w16_off_$n 150 mp '0-100000:100:20:4') > /dev/null 2>&1
      sum w16_off_$n > $W/runs/off_$n.txt 2>&1 ;;
    long)
      (cd $R && lock env NWII_INPUT_PHASES=6 NWII_IDXSNAP_LOG=1 EVERY=1800 PROFILE=free RES=1 \
         pc/run.sh "$bin" w16_longt_$n 240 mp '0-100000:100:20:4') > /dev/null 2>&1
      (cd $R && lock env NWII_INPUT_PHASES=3 NWII_INPUT_SCRIPT_1='60-100000:4:300:30;210-100000:8:300:30' \
         NWII_IDXSNAP_LOG=1 EVERY=1800 PROFILE=free RES=1 pc/run.sh "$bin" w16_longm_$n 180 mp '0-100000:100:20:4') > /dev/null 2>&1
      { sum w16_longt_$n; sum w16_longm_$n; } > $W/runs/long_$n.txt 2>&1 ;;
  esac
  echo "$st done: $W/runs/${st}_$n.txt"
done
