#!/usr/bin/env bash
# perf_x86.sh <escena> [segundos de muestreo=20]
#   Control en x86: corre NSMBW_dx (m2 de PC sin instrumentar) solo con esa demo, en 2 núcleos P (PROFILE=p2), y
#   durante la ventana estable de la demo muestrea con perf, por símbolo, tres contadores del hilo de la CPU:
#   instrucciones, fallos de L1I y fallos de la caché de último nivel. Deja runs/perf_<escena>/{perf.data,sym.txt}.
#   tools/perf_x86.py lo resume (código generado / runtime / bucle ocioso).
set -u
V=/home/djvemo/nsmbw-port/r9/r32dx; E=$1; S=${2:-20}
out=$V/runs/perf_$E; mkdir -p $out
(cd $V && LISTA=$E PROFILE=p2 tools/run_dx.sh bin/NSMBW_dx perfrun_$E 75 > $out/run.out 2>&1) &
# esperar a que la demo esté en su tramo 'main' (línea "[DX] ini ... escena=<E> seg=main")
L=$V/pc/runs/dx_perfrun_$E/run.log
for i in $(seq 1 3000); do grep -q "^\[DX\] ini .*escena=$E seg=main" $L 2>/dev/null && break; sleep 0.2; done
sleep 4
pid=$(pgrep -f "^$V/bin/NSMBW_dx " | head -1)
[[ -n $pid ]] || { echo "no encuentro el proceso" >&2; wait; exit 2; }
perf record -q -e cpu_core/instructions,period=2000003/u -e cpu_core/L1-icache-load-misses,period=4001/u -e cpu_core/cache-misses,period=1009/u -e cpu_core/branch-misses,period=1009/u \
  -p $pid -o $out/perf.data -- sleep $S 2> $out/perf.err
wait
perf script -i $out/perf.data -F tid,event,sym 2> /dev/null > $out/sym.txt
grep "^\[FPS\]" $V/runs/perfrun_$E/fps.txt | tail -5 > $out/fps.txt
wc -l $out/sym.txt
