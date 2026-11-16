#!/usr/bin/env bash
# Guardia de memoria (31-oct): mientras el usuario usa programas pesados propios (p. ej. demucs de UltraStar),
# baja el freno de nsmbw.slice a 4,5 GB para que, si falta RAM, systemd-oomd corte pruebas del port y no lo suyo.
# Arranque: systemd-run --user --unit=nsmbw-memguard ~/nsmbw-port/scripts/mem_guard.sh
cur=""
while :; do
  if pgrep -f "python -m demucs" >/dev/null; then want=4500M; else want=5750M; fi
  if [[ $want != "$cur" ]]; then
    systemctl --user set-property --runtime nsmbw.slice MemoryHigh=$want && cur=$want && echo "nsmbw.slice MemoryHigh=$want"
  fi
  sleep 15
done
