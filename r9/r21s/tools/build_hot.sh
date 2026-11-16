#!/usr/bin/env bash
# build_hot.sh <name> <profiles...>   (correr con ~/nsmbw-port/scripts/lim; el nombre build_*.sh toma turno de compilación)
# Frente L (r9/r16l) aplicado a r18m: re-enlaza la NRO de Switch <name> (enlace guardado por build_m.sh en
# nx/link_<name>.cmd) 1) sin orden y con mapa, 2) con el orden "lo caliente primero" de r9/tools/hot_order.py
# hecho con los perfiles dados (PERFIL@ELF[:PESO]), comprueba (hot_order_eval/eqcheck) y saca
#   bin/nsmbw_<name>_hot.nro/.elf  (+ _input_test.nro con el mismo orden; solo pruebas)
set -euo pipefail
M=/home/djvemo/nsmbw-port/r9/r18m; T=/home/djvemo/nsmbw-port/r9/tools
BASE=${BASE:-/home/djvemo/nsmbw-port/r9/r14g5}; AB=$BASE/nx/build
DKP=/opt/devkitpro; ICON=$DKP/libnx/default_icon.jpg
n=$1; shift
cmd=$(cat "$M/nx/link_$n.cmd")
D=$M/nx/hot/$n; mkdir -p "$D/base" "$D/hot" "$D/hot_test" "$M/nx/hot/order"
WORKER=(--static-root '_ZZL11StartWorkervENUlPvE_4_FUNES_' --static-root '_ZZL10WorkerMainv')
# 1) sin orden, con mapa
(cd "$AB" && eval "${cmd/-o nsmbw.elf/-o $D/base/nsmbw.elf -Wl,-Map=$D/base/nsmbw.map}")
cmp <(/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objcopy -O binary -j .text "$D/base/nsmbw.elf" /dev/stdout | sha256sum) \
    <(/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objcopy -O binary -j .text "$M/bin/nsmbw_$n.elf" /dev/stdout | sha256sum) \
  && echo "[hot] enlace base: .text idéntico a bin/nsmbw_$n.elf"
# 2) orden
O=$M/nx/hot/order/$n
python3 $T/hot_order.py --map "$D/base/nsmbw.map" --elf "$D/base/nsmbw.elf" --out "$O.txt" --report "${O}_informe.txt" \
  --by-thread "${WORKER[@]}" "$@" | tee "${O}_log.txt"
(cd "$AB" && eval "${cmd/-o nsmbw.elf/-o $D/hot/nsmbw.elf -Wl,--section-ordering-file=$O.txt -Wl,-Map=$D/hot/nsmbw.map}")
# 3) comprobaciones (mismas secciones, datos en su sitio; código igual salvo reubicaciones)
P1=${EVAL_PROFILE:-}
python3 $T/hot_order_eval.py --base-map "$D/base/nsmbw.map" --base-elf "$D/base/nsmbw.elf" \
  --new-map "$D/hot/nsmbw.map" --new-elf "$D/hot/nsmbw.elf" $P1 | tee "${O}_eval.txt"
python3 $T/hot_order_eqcheck.py "$D/base/nsmbw.elf" "$D/hot/nsmbw.elf" | tee "${O}_eqcheck.txt"
# 4) NRO
$DKP/tools/bin/nacptool --create "NSMBW PRUEBA m1 (todo junto)" "nsmbw-port" "m1" "$D/hot/nsmbw.nacp" >/dev/null
$DKP/tools/bin/elf2nro "$D/hot/nsmbw.elf" "$M/bin/nsmbw_${n}_hot.nro" --icon=$ICON --nacp="$D/hot/nsmbw.nacp" >/dev/null
cp -f "$D/hot/nsmbw.elf" "$M/bin/nsmbw_${n}_hot.elf"
# 5) _input_test con el mismo orden (solo pruebas guionizadas)
INPUTO=$M/nx/build/input_script_nx.o
tcmd=${cmd/-o nsmbw.elf/-o $D/hot_test/nsmbw.elf -Wl,--section-ordering-file=$O.txt}
tcmd=${tcmd/ -fPIE / -fPIE $INPUTO -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }
(cd "$AB" && eval "$tcmd")
$DKP/tools/bin/elf2nro "$D/hot_test/nsmbw.elf" "$M/bin/nsmbw_${n}_hot_input_test.nro" --icon=$ICON --nacp="$D/hot/nsmbw.nacp" >/dev/null
(cd "$M/bin" && sha256sum "nsmbw_${n}_hot.nro" "nsmbw_${n}_hot.elf" "nsmbw_${n}_hot_input_test.nro")
