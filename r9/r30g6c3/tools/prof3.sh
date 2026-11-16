#!/usr/bin/env bash
# prof3.sh: hostprof de las corridas de consola de e2 / c1 / c2 (lista, 10-nov) -> diag/hostprof_*.txt
cd /home/djvemo/nsmbw-port/r9/r30g6c3
L=/home/djvemo/nsmbw-port/switch/real_logs
run() { python3 -I tools/hostprof.py --elf $2 --top 80 $3/profile_cpu.txt > diag/hostprof_$1.txt 2>&1; }
run e2 ../r21s/bin/nsmbw_g6e2s.elf $L/bench_L03_g6e2s_lista_rep_1110_1000 &
run c1 ../r23g6c/bin/nsmbw_g6c1s.elf $L/bench_L01_g6c1s_lista_1110_0944 &
run c2 ../r23g6c/bin/nsmbw_g6c2s.elf $L/bench_L02_g6c2s_lista_1110_0952 &
wait
