#!/usr/bin/env bash
# perf_snd.sh <bin> <nombre> <demo> [env...]: corrida de PC propia (tools/pcrun.sh, 115 s) y perf del proceso
# (buscado por su carpeta de trabajo) 40 s desde el s 55 a 4 kHz; el hilo de sonido es el que tiene snd_pump.
set -u
A=/home/djvemo/nsmbw-port/r9/r43a
bin=$(realpath "$1"); name=$2; demo=$3; shift 3
o=$A/runs/$name; rm -rf $o; mkdir -p $o
( cd $A && flock pc/.runlock tools/pcrun.sh "$bin" $name 115 NWII_NO_SAVE=1 NWII_FORCE_DEMO=$demo NWII_FORCE_DEMO_LOOP=1 "$@" ) &
pid=
for i in $(seq 1 600); do
  for p in $(pgrep -f "$(basename $bin) /home/djvemo/nsmbw-port/game/SMNE01"); do
    [[ $(readlink /proc/$p/cwd) == $A/pc/runs/work_$name ]] && [[ $(cat /proc/$p/comm) != timeout ]] && pid=$p
  done
  [[ -n $pid ]] && break; sleep 1
done
echo "pid=$pid"; sleep 55
perf record -F 4000 -p "$pid" -o $o/perf.data -- sleep 40 > /dev/null 2>&1
wait
cp $A/pc/runs/$name/run.log $o/run.log
tid=$(perf report -i $o/perf.data --no-children --sort pid,sym --stdio 2>/dev/null | grep -E 'snd_pump|snd_thread_main' | head -1 | awk '{print $2}' | cut -d: -f1)
echo "tid=$tid" | tee $o/tid.txt
perf report -i $o/perf.data --no-children --sort pid --stdio 2>/dev/null | grep -v '^#' | grep . | head > $o/hilos.txt
perf report -i $o/perf.data --no-children --tid=$tid --sort sym --stdio --percentage relative 2>/dev/null | grep -v '^#' | grep . | head -80 > $o/snd_sym.txt
