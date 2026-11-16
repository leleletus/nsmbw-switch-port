#!/usr/bin/env bash
# prof_escena.sh <bin_input_test> <name> <demo> [perf_secs=20] [extra env...]: perf en PC de una demo (todos los hilos)
# (copia de r36r/tools/prof_castillo.sh con la demo como parametro; escribe en r9/r39r2)
set -u
R=/home/djvemo/nsmbw-port/r9/r39r2; O=$R/prof
bin=$(realpath "$1"); name=$2; demo=$3; ps=${4:-20}; shift 4 || true
( cd $R && env NWII_FORCE_DEMO=$demo NWII_FORCE_DEMO_LOOP=1 NWII_NO_SAVE=1 "$@" EVERY=0 PROFILE=${PROFILE:-free} RES=${RES:-1.5789} \
   pc/run.sh "$bin" "$name" 90 mp '0-90:100:20:4' > /dev/null 2>&1 ) &
rp=$!
log=$R/pc/runs/$name/run.log
for i in $(seq 300); do grep -qa "start $demo" "$log" 2>/dev/null && break; sleep 0.5; done
sleep 8
gpid=$(pgrep -n -f "^$bin " || true); echo "pid=$gpid"
perf record -F 2999 -g -p "$gpid" -o "$O/$name.data" -- sleep "$ps" 2> "$O/$name.perf.err"
wait $rp
perf report -i "$O/$name.data" --no-children --sort comm,sym --stdio 2>/dev/null | grep -v "^$" | grep "%" | head -120 > "$O/$name.txt"
head -60 "$O/$name.txt"
