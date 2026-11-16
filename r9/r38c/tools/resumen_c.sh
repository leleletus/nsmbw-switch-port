#!/usr/bin/env bash
# resumen_c.sh <bench_dir>...: lo que hay que mirar de cada corrida del frente C (ventanas, demos, relojes, imagen,
# ms juego REAL, fiesta, hilos/audio, lineas [C38] y [VIGIA]).
T=/home/djvemo/nsmbw-port/r9
for d in "$@"; do
  d=${d%/}; L=$d/nwii.log
  echo "################ $(basename $d)"
  grep -h "windows, mean\|IMAGEN\|CRASH\|CONGELADA\|RELOJ\|FATAL" $d/resumen.txt 2>/dev/null
  echo "demos: $(grep -a -c '^\[DEMO\] start' $L)  ultima: $(grep -a '^\[DEMO\] start' $L | tail -1)"
  echo "relojes: $(grep -a -o 'cpu [0-9]* MHz, gpu [0-9]* MHz' $L | sort | uniq -c | tr '\n' ' ')"
  grep -a -m3 "S2: CPU de sonido\|render worker thread\|VIGIA\] vigilante\|sonido en otro nucleo" $L | cut -c1-200
  python3 $T/r31t/tools/por_escena_real.py $d 2>/dev/null | awk '/^== /{e=$2} /^bench_/{printf "  %-18s dib %5s juego %5s ms_juego_REAL %5s render %5s\n", e, $2, $3, $5, $7}'
  python3 $T/r36r/tools/fiesta.py $d 2>/dev/null | cut -c36-
  python3 $T/r38c/tools/hilos.py $d | tail -n +2
  grep -a '^\[C38\] prio' $L | tail -1 | cut -c1-300
  grep -a "^\[VIGIA\] #\|\[VIGIA\] S2:\|\[VIGIA\] gx\|\[VIGIA\] cpu" $L | head -8 | cut -c1-400
done
