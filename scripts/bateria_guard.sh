#!/usr/bin/env bash
# Guardia de batería (30-oct): con todo a máximo rendimiento el PC gasta más de lo que entrega el
# cargador; la batería bajó del 7 % a 0 y el sistema se suspendió (14:03), parando la ronda horas.
# Si la batería se está descargando y llega al 8 %, congela nsmbw.slice (compilaciones y pruebas
# lanzadas con scripts/lim) para que el cargador recupere; la descongela al llegar al 15 %.
# Arranque: systemd-run --user --unit=nsmbw-bateria ~/nsmbw-port/scripts/bateria_guard.sh · registro: journalctl --user -u nsmbw-bateria
B=/sys/class/power_supply/BAT1
frozen=0
while :; do
  cap=$(cat $B/capacity); st=$(cat $B/status)
  if (( !frozen )) && [[ $st == Discharging ]] && (( cap <= 8 )); then
    systemctl --user freeze nsmbw.slice && frozen=1 && echo "batería $cap % ($st): nsmbw.slice CONGELADO"
  elif (( frozen )) && (( cap >= 15 )); then
    systemctl --user thaw nsmbw.slice && frozen=0 && echo "batería $cap %: nsmbw.slice reanudado"
  fi
  sleep 20
done
