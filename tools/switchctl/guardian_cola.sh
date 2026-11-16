#!/bin/bash
# guardian_cola.sh: relanza la cola si hay trabajos pendientes y el servicio no corre (la cola sale al vaciarse).
# No arranca nada si existe .ocupado (pausa del usuario) o .pausa_usuario. Sale e imprime al ver un incidente.
Q=/home/djvemo/nsmbw-port/tools/switchctl; F=$Q/cola_hecho.txt; N=$(wc -l < $F)
while true; do
  sleep 30
  M=$(wc -l < $F)
  if [ "$M" -gt "$N" ]; then NEW=$(tail -n +$((N+1)) $F); N=$M
    if echo "$NEW" | grep -q -E "SOSPECHOSA|RELOJ|rc [1-9]|CRASH|DETENIDA${PAT:+|$PAT}"; then echo "EVENTO $(date +%H:%M):"; echo "$NEW"; exit 0; fi
  fi
  if ! systemctl --user is-active -q nsmbw-cola && [ ! -f $Q/.ocupado ] && [ ! -f $Q/.pausa_usuario ] && ls $Q/.cola | grep -q '\.job$'; then
    systemctl --user reset-failed nsmbw-cola 2>/dev/null
    systemd-run --user --unit=nsmbw-cola --collect --quiet -p StandardOutput=append:$Q/cola_run.log -p StandardError=append:$Q/cola_run.log $Q/cola.sh run
    echo "$(date +%H:%M) cola relanzada" >> $Q/guardian.log
  fi
done
