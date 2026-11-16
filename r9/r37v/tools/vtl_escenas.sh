#!/usr/bin/env bash
# vtl_escenas.sh <bench> <vtl.bin>: resumen de la linea de tiempo por escena (tiempos de [DEMO] start/end del registro,
# con 6 s de margen al principio y 3 al final; t0 de la linea de tiempo ~ t0 del registro)
B=$1; F=$2; T=/home/djvemo/nsmbw-port/r9/r37v/tools/vtl.py
grep -a "^\[DEMO\] start\|^\[DEMO\] end" $B/nwii.log | awk '/start/{n=$3; s=$5} /end/{if($3==n) print n, s+6, $5-3}' | while read n a b; do
  echo "=== $n ($a-$b s)"; python3 $T $F resumen --desde $a --hasta $b | grep -v "VI lanzados"; done
