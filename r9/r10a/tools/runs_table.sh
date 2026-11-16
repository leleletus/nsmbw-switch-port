#!/usr/bin/env bash
# runs_table.sh <run dirs...>: fps, guest core %, ms per frame, warnings of citron_scene.sh runs
for d in "$@"; do
  [[ -d $d ]] || continue
  [[ -f $d/summary.txt ]] || { echo "$(basename $d): no summary"; continue; }
  s=$d/summary.txt
  fps=$(grep -m1 '^fps (5 s windows)' $s | awk '{print $6}')
  c1=$(grep -m1 'CPUCore_1 per game frame' $s | awk '{print $5}')
  c0=$(grep -m1 'CPUCore_0 per game frame' $s | awk '{print $5}')
  p1=$(grep -m1 '^   CPUCore_1 ' $s | awk '{print $2}')
  p0=$(grep -m1 '^   CPUCore_0 ' $s | awk '{print $2}')
  rp=$(grep -m1 '^\[RPROF\]' $s | grep -o 'render_us/frame [0-9]*')
  ax=$(grep -m1 -o '\[R9\] axfx native=[0-9]* generated(EE set)=[0-9]*' $d/nwii.log 2>/dev/null)
  ld=$(grep -m1 -o 'load_before=[0-9.]*' $d.log 2>/dev/null)
  t=$(grep -m1 -o 'package temp mean [0-9]* C' $s)
  echo "$(basename $d): $ld $t fps=$fps core1=$p1 (${c1} ms/f) core0=$p0 (${c0} ms/f) $rp warn=$(grep -c WARNING $s) $ax"
done
