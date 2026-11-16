#!/usr/bin/env bash
# [r40d] prof_agua.sh <bin_input_test> <name> [perf_secs=25] [extra env...]: PC perf (worker thread) of agua_pinguino,
# heavy part (from ~22 s into the demo). Output prof/<name>.txt (self %, worker only).
set -u
R=/home/djvemo/nsmbw-port/r9/r40d; O=$R/prof
bin=$(realpath "$1"); name=$2; ps=${3:-25}; shift 3 || true
( cd $R && flock pc/.runlock env NWII_FORCE_DEMO=agua_pinguino NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 "$@" EVERY=0 PROFILE=free \
   pc/run.sh "$bin" "$name" $((ps + 45)) mp "0-$((ps + 45)):100:20:4" > /dev/null 2>&1 ) &
rp=$!
log=$R/pc/runs/$name/run.log
for i in $(seq 400); do grep -qa "start agua" "$log" 2>/dev/null && break; sleep 0.5; done
sleep 22
gpid=$(pgrep -n -f "^$bin /home" || true); echo "pid=$gpid"; ps -o pid,args -p "$gpid" | tail -1 | cut -c1-120
perf record -F 1999 ${PERFCG:--g} -p "$gpid" -o "$O/$name.data" -- sleep "$ps" 2> "$O/$name.perf.err"
wait $rp
# the worker = the tid with the most samples in PrepFrame's callees
perf report -i "$O/$name.data" --children --sort tid --stdio 2>/dev/null | grep -v "^#" | grep "%" | head -12 > "$O/$name.tids.txt"
echo done
