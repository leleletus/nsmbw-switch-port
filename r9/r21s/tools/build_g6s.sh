#!/usr/bin/env bash
# build_g6s.sh <e1|e2>   (correr con ~/nsmbw-port/scripts/lim; solo enlaza, no compila)
# r21s: g6<e>s = código generado de G6 etapa <e> (r9/r20g6/<e>/{nx_obj,pc_obj}, solo lectura) + runtime de s1
# (m1 + forcedemo.patch, ya compilado en r21s/nx/build y r21s/pc_build). Es la línea de enlace que usó G6
# (r20g6/nx_link_g6<e>.cmd / pc_link_g6<e>.cmd = la de m1 con los output_*.o de G6) con las 3 piezas del
# runtime de m1 (main.o, libnwiiruntime.a, libglad.a) cambiadas por las de s1, y en la Switch el orden L de
# G6 (r20g6/nx/hot/order/g6<e>.txt, copiado en nx/hot/order/).
#   -> bin/nsmbw_g6<e>s.nro/.elf (el juego, GL) + _input_test.nro (solo pruebas) + NSMBW_g6<e>s(_input_test) (PC)
set -euo pipefail
S=/home/djvemo/nsmbw-port/r9/r21s; G=/home/djvemo/nsmbw-port/r9/r20g6; G5=/home/djvemo/nsmbw-port/r9/r14g5
E=${1:?e1|e2}; DKP=/opt/devkitpro; ICON=$DKP/libnx/default_icon.jpg; N=g6${E}s
D=$S/nx/$N; mkdir -p "$D/game" "$D/test" "$S/bin"
# Switch
cmd=$(sed 's#/r9/r18m/#/r9/r21s/#g' $G/nx_link_g6$E.cmd)
[[ $cmd != *r18m* ]] || { echo "$N: queda algo de r18m en el enlace" >&2; exit 3; }
[[ $(tr ' ' '\n' <<<"$cmd" | grep -c "$G/$E/nx_obj/output_") == 87 ]] || { echo "$N: faltan objetos de G6" >&2; exit 3; }
echo "$cmd" > $S/nx/link_$N.cmd
O=$S/nx/hot/order/g6$E.txt; cp -f $G/nx/hot/order/g6$E.txt $O
(cd $G5/nx/build && eval "${cmd/-o nsmbw.elf/-o $D/game/nsmbw.elf -Wl,--section-ordering-file=$O}")
$DKP/tools/bin/nacptool --create "NSMBW PRUEBA $N (G6 $E + demos fijas)" "nsmbw-port" "$N" "$D/game/nsmbw.nacp" > /dev/null
$DKP/tools/bin/elf2nro "$D/game/nsmbw.elf" "$S/bin/nsmbw_$N.nro" --icon=$ICON --nacp="$D/game/nsmbw.nacp" > /dev/null
cp -f "$D/game/nsmbw.elf" "$S/bin/nsmbw_$N.elf"
tcmd=${cmd/-o nsmbw.elf/-o $D/test/nsmbw.elf -Wl,--section-ordering-file=$O}
tcmd=${tcmd/ -fPIE / -fPIE $S/nx/build/input_script_nx.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }
(cd $G5/nx/build && eval "$tcmd")
$DKP/tools/bin/elf2nro "$D/test/nsmbw.elf" "$S/bin/nsmbw_${N}_input_test.nro" --icon=$ICON --nacp="$D/game/nsmbw.nacp" > /dev/null
# PC
pcmd=$(sed 's#/r9/r18m/pc_build/#/r9/r21s/pc_build/#g' $G/pc_link_g6$E.cmd)
[[ $pcmd != *r18m* ]] || { echo "$N: queda algo de r18m en el enlace de PC" >&2; exit 3; }
echo "$pcmd" > $S/nx/pc_link_$N.cmd
(cd $G5/export/build && eval "${pcmd/ -o NSMBW / -o $S/bin/NSMBW_$N }")
(cd $G5/export/build && eval "${pcmd/ -o NSMBW / -o $S/bin/NSMBW_${N}_input_test $S/pc_build/input_script.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }")
(cd $S/bin && sha256sum nsmbw_$N.nro nsmbw_$N.elf nsmbw_${N}_input_test.nro NSMBW_$N | tee $S/runs/bin_sha256_$N.txt)
