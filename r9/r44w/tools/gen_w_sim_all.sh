#!/usr/bin/env bash
# [r44w] gen_w_sim_all.sh <export> <salida.inc>: las dos versiones de la simulación de las olas en un .inc:
#   w_sim  (NWII_W_SIM="1"): copia del generado con FPR locales;
#   w_sim2 (NWII_W_SIM="2"): + SinFIdx (y demás llamados hoja de la lista) integrados como lambdas con los FPR locales.
set -eu
E=$1; O=$2; T=$(mktemp -d)
python3 -I "$(dirname "$0")/gen_w_sim.py" "$E" $T/a.inc --fpr
python3 -I "$(dirname "$0")/gen_w_sim.py" "$E" $T/b.inc --fpr --prefix=w_sim2 --inline=80237BD0,8000D0D0,8000D0A0
cat $T/a.inc $T/b.inc > "$O"; rm -rf "$T"
