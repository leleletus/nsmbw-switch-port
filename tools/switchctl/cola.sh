#!/usr/bin/env bash
# cola.sh: one queue of console benchmarks, run one at a time (never two on the console).
#   [ENV...] cola.sh add <file.nro> <name> [seconds=150]
#       ENV (optional, saved with the job): BURST_FROM (20) BURST_SECS (30) SHOT_EVERY NXDEST DEBUG_EXTRA PULL_PREFIX
#   cola.sh run        # process the queue until empty (only one runner: flock); waits for a bench in progress
#   cola.sh list
# Each job is a file .cola/<time>_<name>.job (the env + the bench command); finished: cola_hecho.txt.
# The NRO is copied aside when queued, so a later rebuild of the same path doesn't change what gets measured.
HERE=$(cd "$(dirname "$0")" && pwd)
J=$HERE/.cola
DONE=$HERE/cola_hecho.txt
mkdir -p "$J"
case ${1:-} in
  add)
    nro=$(realpath "$2"); name=$3; secs=${4:-150}
    seq=$(date +%s%N | cut -c4-16)
    cp "$nro" "$J/$name.nro"
    {
      printf 'export BURST_FROM=%q BURST_SECS=%q\n' "${BURST_FROM:-20}" "${BURST_SECS:-30}"
      for v in SHOT_EVERY NXDEST DEBUG_EXTRA PULL_PREFIX NO_PRESS PULL_CMD; do
        [[ -n ${!v:-} ]] && printf 'export %s=%q\n' "$v" "${!v}"
      done
      printf '%q %q %q %q\n' "$HERE/bench_title.sh" "$J/$name.nro" "$name" "$secs"
    } > "$J/${seq}_$name.job"
    echo "en cola: $name" ;;
  list) ls "$J" | grep '\.job$'; echo "--- hechos:"; tail -8 "$DONE" 2>/dev/null ;;
  run)
    exec 9> "$HERE/.cola.lock"
    flock -n 9 || { echo "ya hay un procesador de la cola"; exit 0; }
    while true; do
      job=$(ls "$J" | grep '\.job$' | sort | head -1)
      [[ -z $job ]] && break
      while [[ -f $HERE/.ocupado ]]; do sleep 10; done
      mv "$J/$job" "$J/$job.running"
      name=${job#*_}; name=${name%.job}
      echo "=== $(date +%T) $name"
      bash "$J/$job.running" 2>&1 | tail -40
      rc=${PIPESTATUS[0]}
      res=$(grep -h 'windows, mean\|CRASH' /home/djvemo/nsmbw-port/switch/real_logs/bench_${name}_*/resumen.txt 2>/dev/null | tail -2 | tr '\n' ' ')
      echo "$(date '+%F %T') $name (rc $rc): $res" >> "$DONE"
      mv "$J/$job.running" "$J/$job.done"
      if ((rc == 1 || rc == 4)); then echo "$(date '+%F %T') COLA DETENIDA: $name terminó con rc $rc (problema de la consola o de debug.toml)" >> "$DONE"; break; fi
      sleep 5
    done ;;
  *) echo "uso: cola.sh add|run|list"; exit 1 ;;
esac
