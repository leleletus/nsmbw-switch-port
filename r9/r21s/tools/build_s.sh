#!/usr/bin/env bash
# build_s.sh <nombre>   (correr con ~/nsmbw-port/scripts/lim; BASE=r9/r14g5, DK_LIB=deko3d como m1)
# r21s: NRO de Switch = build_m.sh (runtime de r21s + objetos de G5 v3 + output_24 de A2) y re-enlace con
# el MISMO orden "lo caliente primero" de m1 (r18m/nx/hot/order/m1_dk3drel.txt, copiado en nx/hot/order/).
# Las funciones nuevas (force_demo.cpp) no están en el orden: el enlazador las pone detrás (son frías).
#   -> bin/nsmbw_<nombre>.nro/.elf (el juego; arranca con GL) y bin/nsmbw_<nombre>_input_test.nro (solo pruebas)
set -euo pipefail
S=/home/djvemo/nsmbw-port/r9/r21s
export BASE=${BASE:-/home/djvemo/nsmbw-port/r9/r14g5} DK_LIB=${DK_LIB:-deko3d}
AB=$BASE/nx/build DKP=/opt/devkitpro ICON=$DKP/libnx/default_icon.jpg
n=$1
LIM_SLOT_HELD=1 $S/tools/build_m.sh $S "$n" nx
cmd=$(cat "$S/nx/link_$n.cmd")
O=$S/nx/hot/order/m1_dk3drel.txt
D=$S/nx/hot/$n; mkdir -p "$D/hot" "$D/hot_test" "$S/bin/sin_orden"
mv -f "$S/bin/nsmbw_$n.nro" "$S/bin/nsmbw_$n.elf" "$S/bin/nsmbw_${n}_input_test.nro" "$S/bin/sin_orden/"
(cd "$AB" && eval "${cmd/-o nsmbw.elf/-o $D/hot/nsmbw.elf -Wl,--section-ordering-file=$O}")
$DKP/tools/bin/nacptool --create "NSMBW PRUEBA $n (m1 + demos fijas)" "nsmbw-port" "$n" "$D/hot/nsmbw.nacp" >/dev/null
$DKP/tools/bin/elf2nro "$D/hot/nsmbw.elf" "$S/bin/nsmbw_$n.nro" --icon=$ICON --nacp="$D/hot/nsmbw.nacp" >/dev/null
cp -f "$D/hot/nsmbw.elf" "$S/bin/nsmbw_$n.elf"
INPUTO=$S/nx/build/input_script_nx.o
tcmd=${cmd/-o nsmbw.elf/-o $D/hot_test/nsmbw.elf -Wl,--section-ordering-file=$O}
tcmd=${tcmd/ -fPIE / -fPIE $INPUTO -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }
(cd "$AB" && eval "$tcmd")
$DKP/tools/bin/elf2nro "$D/hot_test/nsmbw.elf" "$S/bin/nsmbw_${n}_input_test.nro" --icon=$ICON --nacp="$D/hot/nsmbw.nacp" >/dev/null
(cd "$S/bin" && sha256sum "nsmbw_$n.nro" "nsmbw_$n.elf" "nsmbw_${n}_input_test.nro" | tee "$S/runs/bin_sha256_$n.txt")
