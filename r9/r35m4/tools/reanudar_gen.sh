#!/usr/bin/env bash
# reanudar_gen.sh: espera a que termine el finish del PGO de g7d en la otra laptop (pgo_g7d_fin) y relanza la cadena
# PGO de m4 (tools/pgo_m4.sh; la instrumentada sigue donde se quedo: ninja incremental en r17pgo/nx/m4_pgogen).
P=$HOME/nsmbw-port; L=$P/r9/r35m4/runs/pgo_m4.log
while :; do
  rc=$($P/scripts/remoto.sh estado 2>/dev/null | python3 -c "import json,sys;j=[x for x in json.load(sys.stdin)['trabajos'] if x['nombre']=='pgo_g7d_fin'];print(j[0]['rc'] if j else '')" 2>/dev/null)
  [[ -n $rc ]] && break; sleep 60
done
echo "[$(date '+%F %T')] pgo_g7d_fin rc=$rc: se relanza la cadena PGO de m4 (PJOBS=${PJOBS:-8})" >> $L
cd $P/r9/r35m4 && exec bash tools/pgo_m4.sh m4 export nWiiRuntime -DG7_NOIC
