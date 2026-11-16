#!/usr/bin/env bash
# prof_castillo.sh <bin_input_test> <name> [perf_secs=20] [extra env...]: PC perf of the castillo demo (all threads)
set -u
R=/home/djvemo/nsmbw-port/r9/r36r; O=/home/djvemo/nsmbw-port/r9/r36r/prof
bin=$(realpath "$1"); name=$2; ps=${3:-20}; shift 3 || true
( cd $R && env NWII_FORCE_DEMO=castillo NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 "$@" EVERY=0 PROFILE=${PROFILE:-free} RES=${RES:-1} \
   pc/run.sh "$bin" "$name" 80 mp '0-80:100:20:4' > /dev/null 2>&1 ) &
rp=$!
log=$R/pc/runs/$name/run.log
for i in $(seq 200); do grep -qa "start castillo" "$log" 2>/dev/null && break; sleep 0.5; done
sleep 8
gpid=$(pgrep -n -f "^$bin " || true); echo "pid=$gpid"
perf record -F 2999 -g -p "$gpid" -o "$O/$name.data" -- sleep "$ps" 2> "$O/$name.perf.err"
wait $rp
perf report -i "$O/$name.data" --no-children --sort comm,sym --stdio 2>/dev/null | grep -v "^$" | grep "%" | head -80 > "$O/$name.txt"
head -40 "$O/$name.txt"
