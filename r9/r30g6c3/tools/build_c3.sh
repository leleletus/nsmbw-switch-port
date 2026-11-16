#!/usr/bin/env bash
# build_c3.sh <c3|c3a|...>   (run with ~/nsmbw-port/scripts/lim; only links)
# r30g6c3: g6<c>m = generated code of this variant (<c>/{nx_obj,pc_obj}) + the runtime of m2 (= r7f, the
# pieces copied in r9/r24m2/rt). Link lines = m2's (r24m2/nx/link_m2.cmd, pc_link_m2.cmd) with the 87
# output_*.o of e2 replaced by these; L order = m2's (r24m2/nx/order/g6e2.txt) with every NAME_g6 body put
# right before NAME (tools/mk_order.py).
#   -> bin/nsmbw_g6<c>m.nro/.elf (the game, GL) + _input_test.nro (tests only) + NSMBW_g6<c>m(_input_test) (PC)
set -euo pipefail
C=/home/djvemo/nsmbw-port/r9/r30g6c3; M=/home/djvemo/nsmbw-port/r9/r24m2; G5=/home/djvemo/nsmbw-port/r9/r14g5
E=${1:?c3...}; DKP=/opt/devkitpro; ICON=$DKP/libnx/default_icon.jpg; N=g6${E}m
D=$C/nx/$N; mkdir -p "$D/game" "$D/test" "$C/bin" "$C/nx/order" "$C/runs"
cmd=$(sed "s#/r9/r20g6/e2/nx_obj/#/r9/r30g6c3/$E/nx_obj/#g" $M/nx/link_m2.cmd)
[[ $cmd != *r20g6* ]] || { echo "$N: queda algo de r20g6 en el enlace" >&2; exit 3; }
[[ $(tr ' ' '\n' <<<"$cmd" | grep -c "$C/$E/nx_obj/output_") == 87 ]] || { echo "$N: faltan objetos" >&2; exit 3; }
echo "$cmd" > $C/nx/link_$N.cmd
O=$C/nx/order/$N.txt
python3 $C/tools/mk_order.py $M/nx/order/g6e2.txt $C/$E/export/g6c_fns.txt $O
(cd $G5/nx/build && eval "${cmd/-o nsmbw.elf/-o $D/game/nsmbw.elf -Wl,--section-ordering-file=$O -Wl,-Map=$D/game/nsmbw.map}")
$DKP/tools/bin/nacptool --create "NSMBW PRUEBA $N (m2 + G6 etapa 3 $E)" "nsmbw-port" "$N" "$D/game/nsmbw.nacp" > /dev/null
$DKP/tools/bin/elf2nro "$D/game/nsmbw.elf" "$C/bin/nsmbw_$N.nro" --icon=$ICON --nacp="$D/game/nsmbw.nacp" > /dev/null
cp -f "$D/game/nsmbw.elf" "$C/bin/nsmbw_$N.elf"
tcmd=${cmd/-o nsmbw.elf/-o $D/test/nsmbw.elf -Wl,--section-ordering-file=$O}
tcmd=${tcmd/ -fPIE / -fPIE $M/rt/nx/input_script_nx.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }
(cd $G5/nx/build && eval "$tcmd")
$DKP/tools/bin/elf2nro "$D/test/nsmbw.elf" "$C/bin/nsmbw_${N}_input_test.nro" --icon=$ICON --nacp="$D/game/nsmbw.nacp" > /dev/null
if [[ -f $C/$E/pc_obj/.ok ]]; then
  pcmd=$(sed "s#/r9/r20g6/e2/pc_obj/#/r9/r30g6c3/$E/pc_obj/#g" $M/nx/pc_link_m2.cmd)
  [[ $pcmd != *r20g6* ]] || { echo "$N: queda algo de r20g6 en el enlace de PC" >&2; exit 3; }
  echo "$pcmd" > $C/nx/pc_link_$N.cmd
  (cd $G5/export/build && eval "${pcmd/ -o NSMBW / -o $C/bin/NSMBW_$N }")
  (cd $G5/export/build && eval "${pcmd/ -o NSMBW / -o $C/bin/NSMBW_${N}_input_test $M/rt/pc/input_script.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }")
fi
(cd $C/bin && sha256sum nsmbw_$N.nro nsmbw_$N.elf nsmbw_${N}_input_test.nro $(ls NSMBW_$N NSMBW_${N}_input_test 2>/dev/null) | tee $C/runs/bin_sha256_$N.txt)
