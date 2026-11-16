#!/usr/bin/env bash
# [r45p] cadena: repro de m6i (solo orden) + p45a completo
cd /home/djvemo/nsmbw-port/r9/r45p
SOLO_ORDEN=1 tools/nro_completo.sh p45m6i nWiiRuntime_m6i_base > build_nro_p45m6i_b.log 2>&1
tools/nro_completo.sh p45a nWiiRuntime > build_nro_p45a.log 2>&1
echo HECHO >> build_nro_p45a.log
