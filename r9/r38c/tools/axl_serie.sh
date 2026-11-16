#!/usr/bin/env bash
# axl_serie.sh <bin> <prefijo> <n> <segundos> [env extra...]: n corridas desierto4j<->agua con la prueba (c) de M
# (PCSCHED + estres del FIFO) + NWII_C_AXLIST_CHECK=1; resumen por corrida en runs/<prefijo>.txt
cd /home/djvemo/nsmbw-port/r9/r38c
B=$1; P=$2; N=$3; S=$4; shift 4
E=(NWII_NO_SAVE=1 NWII_FORCE_DEMO=desierto4j,agua_pinguino NWII_FORCE_DEMO_LOOP=1 NWII_C_PCSCHED=1 NWII_C_STRESS_FIFO_US=300 NWII_C_STRESS_SFIFO=1 NWII_C_AXLIST_CHECK=1)
for i in $(seq 1 $N); do
  flock ../render/pc/.runlock tools/pcrun.sh $B ${P}_$i $S "${E[@]}" "$@"
  L=runs/${P}_$i/run.log
  echo "$P #$i: demos $(grep -a -c '^\[DEMO\] start' $L) | $(grep -a '^\[C38\] prio' $L | tail -1 | grep -o 'lista AX.*') | rotas(lineas) $(grep -a -c 'LISTA AX ROTA' $L) | NullCall $(grep -a -c '^\[NullCall\]' $L) | vigia $(grep -a -c '^\[VIGIA\] #' $L) alarmas $(grep -a -c 'VIGIA\] ALARMA' $L) | AID ult $(grep -a '^\[S2\] ACTIVO' $L | tail -1 | grep -o 'AID [0-9]*') | $(tail -1 $L)" >> runs/$P.txt
done
