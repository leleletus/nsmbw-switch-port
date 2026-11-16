#!/usr/bin/env bash
# pull_gcda.sh <dest folder> [run]: download a PGO training run from the console over FTP (ftpd
# classic must be open in the hbmenu, as bench_title.sh does for the logs).
#   The instrumented NRO writes sdmc:/switch/NSMBWnx/gcda/<run>/*.gcda (<run> = YYMMDD_HHMMSS of
#   the start, one folder per launch) + dumps.txt. Without [run] it takes the newest folder.
#   Files land in <dest>/<run>/. Nothing is deleted on the SD.
#   The .gcda names contain '#' (object path with '/' -> '#'): sent as %23 in the URL.
set -euo pipefail
IP=${SWITCH_IP:-<IP_SWITCH>}
FTP=ftp://$IP:5000
DEST=${1:?usage: pull_gcda.sh <dest folder> [run]}
ROOT=/switch/NSMBWnx/gcda
run=${2:-}
if [[ -z $run ]]; then
  # LIST + last field, as bench_title.sh does (ftpd classic); never let an empty grep kill the script
  run=$( { curl -s --max-time 30 "$FTP$ROOT/" || true; } | tr -d '\r' | awk '{print $NF}' | sed 's|.*/||' | { grep -E '^[0-9]{6}_[0-9]{6}$|^run_' || true; } | sort | tail -1)
  [[ -n $run ]] || { echo "no run folder in $ROOT/" >&2; exit 1; }
fi
mkdir -p "$DEST/$run"
n=0; fails=0
while IFS= read -r f; do
  f=${f##*/}   # some listings give the full path
  [[ -z $f ]] && continue
  enc=${f//#/%23}
  if curl -s --retry 4 --retry-delay 3 --retry-all-errors --max-time 300 -o "$DEST/$run/$f" "$FTP$ROOT/$run/$enc"; then n=$((n + 1)); else echo "failed: $f" >&2; fails=$((fails + 1)); rm -f "$DEST/$run/$f"; fi
done < <( { curl -s --max-time 60 "$FTP$ROOT/$run/" || echo "LIST failed" >&2; } | tr -d '\r' | awk '{print $NF}' | { grep -E '\.gcda$|^dumps\.txt$' || true; })
((fails == 0)) || { echo "ERROR: $fails files failed" >&2; exit 1; }
((n > 0)) || echo "WARNING: nothing downloaded from $ROOT/$run/ (listing empty?)" >&2
echo "pulled $n files of $ROOT/$run/ into $DEST/$run/"
cat "$DEST/$run/dumps.txt" 2>/dev/null || echo "(no dumps.txt: the dump did not finish?)"
