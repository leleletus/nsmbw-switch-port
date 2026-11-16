#!/usr/bin/env bash
# build_g6cs.sh <c1|c2|...>   (correr con ~/nsmbw-port/scripts/lim; solo enlaza)
# r23g6c: g6<c>s = código generado de G6 etapa 3 paso <c> (r23g6c/<c>/{nx_obj,pc_obj}) + runtime de s1
# (r9/r21s: m1 + forcedemo.patch, ya compilado). Igual que r21s/tools/build_g6s.sh: la línea de enlace de
# G6 e2 (r20g6/nx_link_g6e2.cmd = la de m1 con los output_*.o de G6) con los 87 objetos cambiados por los
# de este paso y las 3 piezas del runtime de m1 cambiadas por las de s1; en la Switch el orden L de G6 e2
# con los cuerpos _g6 delante de cada función caliente (tools/mk_order.py).
#   -> bin/nsmbw_g6<c>s.nro/.elf (el juego, GL) + _input_test.nro (solo pruebas) + NSMBW_g6<c>s(_input_test) (PC)
set -euo pipefail
C=/home/djvemo/nsmbw-port/r9/r23g6c; S=/home/djvemo/nsmbw-port/r9/r21s; G=/home/djvemo/nsmbw-port/r9/r20g6
G5=/home/djvemo/nsmbw-port/r9/r14g5
E=${1:?c1|c2...}; DKP=/opt/devkitpro; ICON=$DKP/libnx/default_icon.jpg; N=g6${E}s
D=$C/nx/$N; mkdir -p "$D/game" "$D/test" "$C/bin" "$C/nx/order"
# Switch
cmd=$(sed "s#/r9/r20g6/e2/nx_obj/#/r9/r23g6c/$E/nx_obj/#g; s#/r9/r18m/#/r9/r21s/#g" $G/nx_link_g6e2.cmd)
[[ $cmd != *r18m* && $cmd != *r20g6* ]] || { echo "$N: queda algo de r18m/r20g6 en el enlace" >&2; exit 3; }
[[ $(tr ' ' '\n' <<<"$cmd" | grep -c "$C/$E/nx_obj/output_") == 87 ]] || { echo "$N: faltan objetos" >&2; exit 3; }
echo "$cmd" > $C/nx/link_$N.cmd
O=$C/nx/order/$N.txt
python3 $C/tools/mk_order.py $G/nx/hot/order/g6e2.txt $C/$E/export/g6c_fns.txt $O
(cd $G5/nx/build && eval "${cmd/-o nsmbw.elf/-o $D/game/nsmbw.elf -Wl,--section-ordering-file=$O}")
$DKP/tools/bin/nacptool --create "NSMBW PRUEBA $N (G6 etapa 3 $E + demos fijas)" "nsmbw-port" "$N" "$D/game/nsmbw.nacp" > /dev/null
$DKP/tools/bin/elf2nro "$D/game/nsmbw.elf" "$C/bin/nsmbw_$N.nro" --icon=$ICON --nacp="$D/game/nsmbw.nacp" > /dev/null
cp -f "$D/game/nsmbw.elf" "$C/bin/nsmbw_$N.elf"
tcmd=${cmd/-o nsmbw.elf/-o $D/test/nsmbw.elf -Wl,--section-ordering-file=$O}
tcmd=${tcmd/ -fPIE / -fPIE $S/nx/build/input_script_nx.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }
(cd $G5/nx/build && eval "$tcmd")
$DKP/tools/bin/elf2nro "$D/test/nsmbw.elf" "$C/bin/nsmbw_${N}_input_test.nro" --icon=$ICON --nacp="$D/game/nsmbw.nacp" > /dev/null
# PC (solo si ya están los objetos de PC)
if [[ -f $C/$E/pc_obj/output_0.o ]]; then
  pcmd=$(sed "s#/r9/r20g6/e2/pc_obj/#/r9/r23g6c/$E/pc_obj/#g; s#/r9/r18m/pc_build/#/r9/r21s/pc_build/#g" $G/pc_link_g6e2.cmd)
  [[ $pcmd != *r18m* && $pcmd != *r20g6* ]] || { echo "$N: queda algo de r18m/r20g6 en el enlace de PC" >&2; exit 3; }
  echo "$pcmd" > $C/nx/pc_link_$N.cmd
  (cd $G5/export/build && eval "${pcmd/ -o NSMBW / -o $C/bin/NSMBW_$N }")
  (cd $G5/export/build && eval "${pcmd/ -o NSMBW / -o $C/bin/NSMBW_${N}_input_test $S/pc_build/input_script.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }")
fi
(cd $C/bin && sha256sum nsmbw_$N.nro nsmbw_$N.elf nsmbw_${N}_input_test.nro $(ls NSMBW_$N 2>/dev/null) | tee $C/runs/bin_sha256_$N.txt)
