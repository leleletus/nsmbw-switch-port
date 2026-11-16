#!/usr/bin/env bash
# ab_title.sh <tag> <secs> <bin>...   (run under flock r9/render/pc/.runlock)
#   PC A/B of the guest (CPU) thread in the title demo: one run per binary, in the order given
#   (A B B A), offscreen + silent, private NAND copy (r9/render/pc/run.sh), 1x.
#   env: PROFILE (limc CPU profile, default sw = 2 LP E-cores, Switch-like), EXTRA (env words).
#   Prints per run the mean over the [FPS] lines after the first STAGE (the attract demo):
#   fps, cpu thread %, host %, worker %, wgp writes/s, flushes/s, bytes/flush.
set -u
R=/home/djvemo/nsmbw-port/r9/render
tag=$1; secs=$2; shift 2
i=0
for bin in "$@"; do
  i=$((i + 1))
  name=w16ab_${tag}_$i
  env NWII_INPUT_PHASES=6 NWII_INPUT_SCRIPT_1= NWII_INPUT_SCRIPT_2= NWII_POLL_STATS=1 ${EXTRA:-} \
    EVERY=0 PROFILE=${PROFILE:-sw} RES=1 "$R/pc/run.sh" "$bin" "$name" "$secs" mp '0-100000:100:20:4' > /dev/null 2>&1
  L=$R/pc/runs/$name/run.log
  awk -v b="$(basename "$bin")" -v n="$name" '
    /scene=5 \(STAGE\)/ && !st { st = 1; next }
    st && /^\[FPS/ {
      k++; f += $2
      for (j = 1; j <= NF; j++) {
        if ($j == "cpu" && $(j+1) == "thread") { v = $(j+2); sub("%,?", "", v); c += v }
        if ($j == "host" && $(j+1) == "thread") { v = $(j+2); sub("%,?", "", v); h += v }
        if ($j == "worker") { v = $(j+1); sub("%,?", "", v); w += v }
        if ($j == "wgp") { v = $(j+1); sub("k", "", v); wr += v; v = $(j+3); sub("k", "", v); fl += v
                            v = $(j+5); sub("[(]", "", v); by += v }
      }
    }
    END { if (!k) { print n, b, "no data"; exit }
      printf "%-16s %-26s n=%d fps=%.1f cpu_thread=%.1f%% host=%.1f%% worker=%.1f%% wgp=%.0fk/s flushes=%.1fk/s (%.0f B)\n",
        n, b, k, f/k, c/k, h/k, w/k, wr/k, fl/k, by/k }' "$L"
done
