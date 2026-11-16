#!/usr/bin/env bash
# abba.sh <A.nro> <B.nro> <scene> <secs> <outbase> [pairs=1]
# Alternated citron runs A B B A (pairs=2: A B B A A B B A) with citron_scene.sh (each run takes the
# machine-wide citron lock itself) and a one-line comparison per run at the end.
# EXTRA_A / EXTRA_B: EXTRA_DEBUG files for the A / B runs.
set -uo pipefail
C=$(cd "$(dirname "$0")/.." && pwd)
A=$1 B=$2 SC=$3 SECS=$4 OB=$5 PAIRS=${6:-1}
order=()
for ((k = 0; k < PAIRS; k++)); do order+=(A B B A); done
i=0
for x in "${order[@]}"; do
  i=$((i + 1))
  nro=$A; ex=${EXTRA_A:-}; [[ $x == B ]] && { nro=$B; ex=${EXTRA_B:-}; }
  out=${OB}_${i}${x}
  [[ -s $out/summary.txt ]] && continue
  powerprofilesctl get | grep -q performance || powerprofilesctl set performance
  EXTRA_DEBUG=$ex "$C/citron_scene.sh" "$nro" "$SC" "$SECS" "$out" > "$out.log" 2>&1
done
for d in ${OB}_*[AB]; do
  [[ -f $d/summary.txt ]] || { echo "$d: no summary"; continue; }
  fps=$(grep -m1 '^fps (5 s windows)' "$d/summary.txt" | awk '{print $5}')
  c1=$(grep -m1 'CPUCore_1 per game frame' "$d/summary.txt" | awk '{print $5}')
  c0=$(grep -m1 'CPUCore_0 per game frame' "$d/summary.txt" | awk '{print $5}')
  p1=$(grep -m1 '^   CPUCore_1 ' "$d/summary.txt" | awk '{print $2}')
  p0=$(grep -m1 '^   CPUCore_0 ' "$d/summary.txt" | awk '{print $2}')
  w=$(grep -c WARNING "$d/summary.txt")
  echo "$(basename $d) fps=$fps core1=$p1 ${c1}ms/f core0=$p0 ${c0}ms/f warnings=$w"
done
