#!/usr/bin/env bash
# pc_verif.sh <n> <tag> [env...]: verificacion de PC de una variante (binario bin/NSMBW_<n>) con el entorno dado:
# (1) audio determinista (DET=1: reloj virtual, AX en linea) 120 s con y sin el entorno -> cmp byte a byte;
# (2) FC 5 demos (FRAMECHECK) con el entorno; (3) checks 1x (title/map/walk/lvl11) con el entorno;
# (4) estres de cuelgues (prueba c + NWII_C_STRESS_IRQ_US=100) x2 con el entorno. -> test/verif_<tag>.txt
cd /home/djvemo/nsmbw-port/r9/r43a; N=$1; T=$2; shift 2
O=test/verif_$T.txt; echo "env: $*" > $O
tools/aud.sh $N $T "$@"; cat test/aud_$T.txt >> $O
test/demos_fc.sh bin/NSMBW_${N}_input_test fc_$T 100 "$@" >> $O 2>&1
tools/pc_checks.sh $N $T "$@"; cat test/checks_$T.txt >> $O
tools/estres.sh bin/NSMBW_$N est_$T 2 NWII_C_PCSCHED_CPUS=8,9,10 "$@"; cat test/estres_est_$T.txt >> $O
echo FIN >> $O
