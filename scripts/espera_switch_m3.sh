#!/usr/bin/env bash
# espera_switch_m3.sh: no-AI. Waits until the Switch answers again (sys-botbase), goes to the hbmenu, puts the m3 PGO
# training jobs V01/V02 back in the queue, starts the queue and the chain scripts/cadena_m3_fin.sh.
set -uo pipefail
P=$HOME/nsmbw-port; Q=$P/tools/switchctl
echo "=== inicio $(date "+%F %T")"
cd "$Q"
until timeout 15 python3 switchctl.py ping 2>/dev/null | grep -q botbase; do sleep 20; done
echo "$(date '+%F %T') la Switch responde"; sleep 25
for i in 1 2 3; do NAV_TMP=/tmp/claude-1000/nav_espera.jpg timeout 180 python3 -W ignore nav.py hbmenu && break; sleep 20; done
for j in .cola/*_V01_m3gen_demosA.job.done .cola/*_V02_m3gen_demosB.job.done; do [[ -f $j ]] && mv "$j" "${j%.done}"; done
N0=$(wc -l < cola_hecho.txt); touch "$P/r9/r17pgo/profiles/.marca_m3"
systemctl --user is-active -q nsmbw-cola || systemd-run --user --unit=nsmbw-cola --collect --quiet \
  -p StandardOutput=append:$Q/cola_run.log -p StandardError=append:$Q/cola_run.log $Q/cola.sh run
systemd-run --user --unit=nsmbw-cadena-m3-fin --collect --quiet -E N0=$N0 \
  -p StandardOutput=append:$P/scripts/cadena_m3_fin.log -p StandardError=append:$P/scripts/cadena_m3_fin.log $P/scripts/cadena_m3_fin.sh
echo "$(date '+%F %T') cola y cadena m3 arrancadas"
