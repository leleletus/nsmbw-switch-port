#!/usr/bin/env bash
# comparar.sh <bench name prefix>...: image check, per-scene metric (por_escena_real.py) and castle split (fiesta.py)
cd /home/djvemo/nsmbw-port/switch/real_logs || exit 1
B=(); for p in "$@"; do d=$(ls -d bench_${p}_* 2>/dev/null | tail -1); [[ -n $d ]] && B+=("$d"); done
for d in "${B[@]}"; do echo "$d: $(tail -1 $d/verif_imagen.txt 2>/dev/null)"; done
python3 /home/djvemo/nsmbw-port/r9/r31t/tools/por_escena_real.py "${B[@]}" | grep -v '^$'
python3 /home/djvemo/nsmbw-port/r9/r36r/tools/fiesta.py "${B[@]}"
