#!/usr/bin/env bash
# seq_runs.sh <scene> <secs> <outdir> <label>=<nro> ...  : citron_scene.sh runs in the given order
# (e.g. r8 c1 c2 c2 c1 r8), outputs <outdir>/<scene>_<n><label>/, then a one-line table.
set -uo pipefail
C=$(cd "$(dirname "$0")/.." && pwd)
SC=$1 SECS=$2 OD=$3; shift 3
mkdir -p "$OD"
i=0
for spec in "$@"; do
  i=$((i + 1)); lab=${spec%%=*}; nro=${spec#*=}
  out=$OD/${SC}_${i}${lab}
  [[ -s $out/summary.txt ]] && continue
  [[ $(powerprofilesctl get) == performance ]] || powerprofilesctl set performance
  # wait (max 20 min) for a quieter machine: 1-min load average under MAXLOAD (default 12)
  for ((w = 0; w < 120; w++)); do
    l=$(cut -d' ' -f1 /proc/loadavg); (( ${l%.*} < ${MAXLOAD:-12} )) && break; sleep 10
  done
  "$C/citron_scene.sh" "$nro" "$SC" "$SECS" "$out" > "$out.log" 2>&1
  echo "load_before=$l load_after=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$out.log"
done
"$C/tools/runs_table.sh" "$OD"/${SC}_*
