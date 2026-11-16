#!/usr/bin/env bash
# mix_runs.sh <secs> <outdir> <scene>:<label>=<nro> ...   citron_scene.sh runs in this order
# (each takes the machine-wide citron lock itself), output <outdir>/<scene>_<n><label>/.
set -uo pipefail
C=$(cd "$(dirname "$0")/.." && pwd)
SECS=$1 OD=$2; shift 2
mkdir -p "$OD"
i=0
for spec in "$@"; do
  i=$((i + 1)); sc=${spec%%:*}; rest=${spec#*:}; lab=${rest%%=*}; nro=${rest#*=}
  out=$OD/${sc}_${i}${lab}
  [[ -s $out/summary.txt ]] && continue
  [[ $(powerprofilesctl get) == performance ]] || powerprofilesctl set performance
  l=$(cut -d' ' -f1-3 /proc/loadavg)
  "$C/citron_scene.sh" "$nro" "$sc" "$SECS" "$out" > "$out.log" 2>&1
  echo "load_before=${l%% *} load_after=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$out.log"
done
