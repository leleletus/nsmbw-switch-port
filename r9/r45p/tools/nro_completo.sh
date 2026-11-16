#!/usr/bin/env bash
# [r45p] nro_completo.sh <tag> <src_runtime_local>: NRO como m6i (runtime con WAGUA + objetos k2a43 + pares + orden L con
# los perfiles de M10, los mismos que m6i) y su variante _input_test (tools/nro_input.sh). Resultado en r45p/bin:
#   <tag>rt_pgo.nro (base), <tag>.nro/.elf (como m6i.nro), <tag>_input_test.nro (para el banco con guion).
set -euo pipefail
cd /home/djvemo/nsmbw-port/r9/r45p
TAG=$1; SRC=$2; O=/home/djvemo/nsmbw-port/r9/r41m/order_in/m6i
[[ -n ${SOLO_ORDEN:-} ]] || WAGUA=1 tools/build_nro.sh ${TAG}rt "$SRC"
KOBJ=k2a43 KOBJDIR=/home/djvemo/nsmbw-port/r9/r41m/nx/k2a43 tools/order_p.sh r45p:${TAG}rt $TAG \
  $O/bench_M10_m6e_def_1115_1758_profile_cpu.txt@$O/m6e_pgo.elf \
  $O/bench_M10_m6e_def_1115_1758_profile_main.txt@$O/m6e_pgo.elf \
  $O/bench_M10_m6e_def_1115_1758_profile_gxw.txt@$O/m6e_pgo.elf
tools/nro_input.sh /home/djvemo/nsmbw-port/r9/r45p/nx/$TAG /home/djvemo/nsmbw-port/r9/r45p/src_${TAG}rt $TAG
