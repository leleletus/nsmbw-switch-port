#!/usr/bin/env bash
# [r45p] pc_perf.sh <binario_input_test> <nombre> <pantalla: mapa|selector> [segs_perf=20]
# Corrida de PC (r41m/pc/run.sh: offscreen, NAND privada mp, NWII_NO_SAVE=1) que entra al mapa con el guion de
# menús y, en «selector», pulsa «−» (WPAD 0x1000) 300 fotogramas después de llegar al mapa. 25 s después de
# la fase 1 engancha `perf record -g` al proceso durante segs_perf y deja perf.data + informe por hilo en
# r41m/pc/runs/<nombre>/ (perf_hilos.txt, perf_<hilo>.txt).
set -u
bin=$(realpath "$1"); name=$2; pant=$3; ps=${4:-20}
PC=/home/djvemo/nsmbw-port/r9/r41m/pc; out=$PC/runs/$name
export NWII_NO_SAVE=1 NWII_INPUT_PHASES=3 NWII_INPUT_SCRIPT_1= NWII_INPUT_SCRIPT_2=
[[ $pant == selector ]] && export NWII_INPUT_SCRIPT_1='300-306:1000:100:6'
EVERY=${EVERY:-600} RES=${RES:-1} "$PC/run.sh" "$bin" "$name" $((ps + 110)) mp '0-100000:100:20:4' > /dev/null 2>&1 &
rp=$!
for i in $(seq 1 120); do sleep 1; grep -aq 'phase 1 starts' "$out/run.log" 2>/dev/null && break; done
sleep 25
pid=$(pgrep -x "$(basename "$bin" | cut -c1-15)" | head -1)
[[ -n $pid ]] || { echo "sin proceso"; wait $rp; exit 1; }
perf record -F 999 -g -p "$pid" -o "$out/perf.data" -- sleep "$ps" > /dev/null 2>&1
wait $rp
cd "$out" || exit 1
perf report -i perf.data --no-children --sort comm --stdio 2>/dev/null | grep -v '^#' | grep . | head -20 > perf_hilos.txt
for c in $(awk '$1 ~ /%/ && $1+0 >= 3 {print $2}' perf_hilos.txt); do
  perf report -i perf.data --no-children --comm "$c" --sort sym --stdio -g none 2>/dev/null | grep -v '^#' | grep . | head -40 > "perf_$c.txt"
done
cat perf_hilos.txt
