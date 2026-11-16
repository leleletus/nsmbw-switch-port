#!/usr/bin/env bash
# citron_when_ready.sh <secs> <outdir> <scene>:<label>=<nro> ...
#   Same as mix_runs.sh, but before each run waits until citron can run normally: the
#   nsmbw.slice memory cap is back to normal (MemoryHigh >= 5.5 GB; the demucs guard lowers it to
#   4.5 GB and then citron spends minutes in memset at boot, 31-oct) and no demucs is running.
#   A run whose game never started (no nwii.log) is moved to <outdir>/failed_* and retried once.
set -uo pipefail
C=$(cd "$(dirname "$0")/.." && pwd)
SECS=$1 OD=$2; shift 2
mkdir -p "$OD"
ready() {
  local mh; mh=$(systemctl --user show nsmbw.slice -p MemoryHigh --value 2>/dev/null)
  [[ $mh == infinity ]] && mh=99999999999
  (( ${mh:-0} >= 5500000000 )) || return 1
  pgrep -f 'python -m demucs' > /dev/null && return 1
  return 0
}
i=0
for spec in "$@"; do
  i=$((i + 1)); sc=${spec%%:*}; rest=${spec#*:}; lab=${rest%%=*}; nro=${rest#*=}
  out=$OD/${sc}_${i}${lab}
  for try in 1 2; do
    [[ -s $out/summary.txt && -f $out/nwii.log ]] && break
    until ready; do sleep 30; done
    [[ $(powerprofilesctl get) == performance ]] || powerprofilesctl set performance
    l=$(cut -d' ' -f1-3 /proc/loadavg)
    echo "$(date +%T) start $out (try $try, load $l)" >> "$OD/chain.log"
    "$C/citron_scene.sh" "$nro" "$sc" "$SECS" "$out" > "$out.log" 2>&1
    echo "load_before=${l%% *} load_after=$(cut -d' ' -f1-3 /proc/loadavg)" >> "$out.log"
    if [[ ! -f $out/nwii.log ]]; then
      echo "$(date +%T) $out: game did not start" >> "$OD/chain.log"
      rm -rf "$OD/failed_${sc}_${i}${lab}_$try"; mv "$out" "$OD/failed_${sc}_${i}${lab}_$try"
    fi
  done
done
echo "$(date +%T) chain done" >> "$OD/chain.log"
