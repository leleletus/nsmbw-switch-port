#!/usr/bin/env bash
# [r45p] estres_p.sh (copia de r41m/tools/estres_m6e.sh, resultado en r45p/test; bin con ruta absoluta)
# [r41m] estres_m6e.sh <bin> <tag> <n> [env...]: n corridas de 150 s de la prueba (c) (PCSCHED + estrés del FIFO de C03,
# desierto4j<->agua) + estrés de bucle de C (NWII_C_STRESS_IRQ_US=100) + NWII_C_AXLIST_CHECK=1, con los valores por
# defecto de m6e. PCSCHED en las CPUs 5,6,7 (C usa 2,3,4 a la vez). -> test/estres_<tag>.txt
cd /home/djvemo/nsmbw-port/r9/r41m; B=$1; T=$2; N=$3; shift 3
E=(NWII_NO_SAVE=1 NWII_FORCE_DEMO=desierto4j,agua_pinguino NWII_FORCE_DEMO_LOOP=1 NWII_C_PCSCHED=1 NWII_C_PCSCHED_CPUS=5,6,7
   NWII_C_STRESS_FIFO_US=300 NWII_C_STRESS_SFIFO=1 NWII_C_STRESS_IRQ_US=100 NWII_C_AXLIST_CHECK=1)
O=/home/djvemo/nsmbw-port/r9/r45p/test/estres_$T.txt; echo "env: ${E[*]} $*" > $O
for i in $(seq 1 $N); do
  flock pc/.runlock tools/pcrun.sh $B ${T}_$i 150 "${E[@]}" "$@"; L=pc/runs/${T}_$i/run.log
  echo "#$i: demos $(grep -a -c '^\[DEMO\] start' $L) | $(grep -a '^\[C38\] prio' $L | tail -1 | grep -o 'lista AX.*') | rotas $(grep -a -c 'LISTA AX ROTA' $L) | NullCall $(grep -a -c '^\[NullCall\]' $L) | vigia $(grep -a -c '^\[VIGIA\] #' $L) alarmas $(grep -a -c 'VIGIA\] ALARMA' $L) | AID ult $(grep -a '^\[S2\] ACTIVO' $L | tail -1 | grep -o 'AID [0-9]*') | fps $(grep -a '^\[FPS' $L | awk '{s+=$2;n++} END{printf "%.1f", s/n}') | $(grep -a -m1 '^\[M6\]' $L | cut -c1-60) | $(tail -1 $L)" >> $O
done
echo FIN >> $O
