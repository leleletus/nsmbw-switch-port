#!/usr/bin/env bash
# build_m2.sh   (correr con ~/nsmbw-port/scripts/lim; solo enlaza, no compila nada del juego ni del runtime)
# r24m2: m2 = runtime de r7f (r9/r22r7, piezas ya compiladas copiadas en rt/) + código generado de G6 e2
# (r9/r20g6/e2/{nx_obj,pc_obj}, solo lectura) + el orden L que usó g6e2s (r20g6/nx/hot/order/g6e2.txt).
# Igual que r21s/tools/build_g6s.sh, pero con las piezas del runtime de r7f en vez de las de s1.
#   0) comprobación: con las copias de rt/ se re-enlaza r7f (Switch) y r7e (PC; en PC r7f = r7e) y debe salir
#      (el .elf se llama nsmbw.elf: libnx guarda el nombre del archivo de salida en .nx-module-name)
#      idéntico byte a byte a r22r7/bin/nsmbw_r7f.elf y r22r7/bin/NSMBW_r7e (prueba de que rt/ es r7f)
#   1) Switch: bin/nsmbw_m2.nro/.elf (el juego, GL) + bin/nsmbw_m2_input_test.nro (solo pruebas)
#   2) PC: bin/NSMBW_m2 + bin/NSMBW_m2_input_test
set -euo pipefail
M=/home/djvemo/nsmbw-port/r9/r24m2; G=/home/djvemo/nsmbw-port/r9/r20g6; G5=/home/djvemo/nsmbw-port/r9/r14g5
S7=/home/djvemo/nsmbw-port/r9/r22r7
DKP=/opt/devkitpro; ICON=$DKP/libnx/default_icon.jpg; N=m2
RT=$M/rt; D=$M/nx/$N; CK=$M/nx/check; mkdir -p "$D/game" "$D/test" "$CK/r7f" "$M/bin"
X18=/home/djvemo/nsmbw-port/r9/r18m/nx/build
X7=$S7/nx/build
toks() { tr ' ' '\n' <<<"$1" | grep -v -e '^$' -e '/output_[0-9]*\.o$' -e 'mod_output_24\.o$'; }

# ---- 0) Switch: r7f reproducido con las copias de rt/ ----
rcmd=$(cat $M/nx/ref/link_r7f_dk3drel.cmd)
rcmd=${rcmd/$X7\/CMakeFiles\/nsmbw.dir\/home\/djvemo\/nsmbw-port\/r9\/r22r7\/nWiiRuntime\/src\/core\/main.o/$RT/nx/main.o}
rcmd=${rcmd/$X7\/runtime\/nwiiruntime\/libnwiiruntime.a/$RT/nx/libnwiiruntime.a}
rcmd=${rcmd/$X7\/_deps\/glad-build\/libglad.a/$RT/nx/libglad.a}
rcmd=${rcmd/$X7\/mod_output_24.o/$RT/nx/mod_output_24.o}
[[ $rcmd != *r22r7* ]] || { echo "$N: queda algo de r22r7 en el enlace de comprobación" >&2; exit 3; }
(cd $G5/nx/build && eval "${rcmd/-o nsmbw.elf/-o $CK/r7f/nsmbw.elf -Wl,--section-ordering-file=$M/nx/ref/r7f_dk3drel.txt}")
if cmp -s $CK/r7f/nsmbw.elf $S7/bin/nsmbw_r7f.elf; then echo "[m2] rt/ reproduce nsmbw_r7f.elf byte a byte"
else echo "[m2] ERROR: rt/ NO reproduce nsmbw_r7f.elf" >&2; exit 5; fi

# ---- 1) Switch: m2 ----
cmd=$(cat $M/nx/ref/nx_link_g6e2.cmd)
cmd=${cmd/$X18\/CMakeFiles\/nsmbw.dir\/home\/djvemo\/nsmbw-port\/r9\/r18m\/nWiiRuntime\/src\/core\/main.o/$RT/nx/main.o}
cmd=${cmd/$X18\/runtime\/nwiiruntime\/libnwiiruntime.a/$RT/nx/libnwiiruntime.a}
cmd=${cmd/$X18\/_deps\/glad-build\/libglad.a/$RT/nx/libglad.a}
[[ $cmd != *r18m* ]] || { echo "$N: queda algo de r18m en el enlace" >&2; exit 3; }
[[ $(tr ' ' '\n' <<<"$cmd" | grep -c "$G/e2/nx_obj/output_") == 87 ]] || { echo "$N: faltan objetos de G6" >&2; exit 3; }
# sin los objetos del juego, la línea de m2 debe ser exactamente la de r7f
if diff <(toks "$cmd") <(toks "$rcmd") > $CK/linea_m2_vs_r7f.diff; then echo "[m2] línea sin output_*.o = la de r7f"
else echo "[m2] ERROR: la línea de m2 difiere de la de r7f fuera del código generado" >&2; cat $CK/linea_m2_vs_r7f.diff >&2; exit 6; fi
echo "$cmd" > $M/nx/link_$N.cmd
O=$M/nx/order/g6e2.txt
(cd $G5/nx/build && eval "${cmd/-o nsmbw.elf/-o $D/game/nsmbw.elf -Wl,--section-ordering-file=$O -Wl,-Map=$D/game/nsmbw.map}")
$DKP/tools/bin/nacptool --create "NSMBW PRUEBA m2 (r7f + G6 e2)" "nsmbw-port" "$N" "$D/game/nsmbw.nacp" > /dev/null
$DKP/tools/bin/elf2nro "$D/game/nsmbw.elf" "$M/bin/nsmbw_$N.nro" --icon=$ICON --nacp="$D/game/nsmbw.nacp" > /dev/null
cp -f "$D/game/nsmbw.elf" "$M/bin/nsmbw_$N.elf"
tcmd=${cmd/-o nsmbw.elf/-o $D/test/nsmbw.elf -Wl,--section-ordering-file=$O}
tcmd=${tcmd/ -fPIE / -fPIE $RT/nx/input_script_nx.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }
(cd $G5/nx/build && eval "$tcmd")
$DKP/tools/bin/elf2nro "$D/test/nsmbw.elf" "$M/bin/nsmbw_${N}_input_test.nro" --icon=$ICON --nacp="$D/game/nsmbw.nacp" > /dev/null

# ---- 0b) PC: r7e (= r7f en PC) reproducido con las copias de rt/ ----
pr=$(cd $G5/export/build && ninja -t commands NSMBW | tail -1 | sed "s/^: && //; s/ && :$//")
pr=${pr/-Wl,--dependency-file=CMakeFiles\/NSMBW.dir\/link.d/}
pr=${pr/CMakeFiles\/NSMBW.dir\/output_24.cpp.o/$RT/pc/mod_output_24.o}
pr=${pr/CMakeFiles\/NSMBW.dir\/nWiiRuntime\/src\/core\/main.cpp.o/$RT/pc/main.cpp.o}
pr=${pr/nWiiRuntime\/libnwiiruntime.a/$RT/pc/libnwiiruntime.a}
(cd $G5/export/build && eval "${pr/ -o NSMBW / -o $CK/NSMBW_r7e }")
if cmp -s $CK/NSMBW_r7e $S7/bin/NSMBW_r7e; then echo "[m2] rt/pc reproduce NSMBW_r7e byte a byte"
else echo "[m2] AVISO: rt/pc no reproduce NSMBW_r7e byte a byte (ver $CK)" >&2; fi

# ---- 2) PC: m2 ----
pcmd=$(cat $M/nx/ref/pc_link_g6e2.cmd)
pcmd=${pcmd/\/home\/djvemo\/nsmbw-port\/r9\/r18m\/pc_build\/CMakeFiles\/NSMBW.dir\/nWiiRuntime\/src\/core\/main.cpp.o/$RT/pc/main.cpp.o}
pcmd=${pcmd/\/home\/djvemo\/nsmbw-port\/r9\/r18m\/pc_build\/nWiiRuntime\/libnwiiruntime.a/$RT/pc/libnwiiruntime.a}
[[ $pcmd != *r18m* ]] || { echo "$N: queda algo de r18m en el enlace de PC" >&2; exit 3; }
echo "$pcmd" > $M/nx/pc_link_$N.cmd
(cd $G5/export/build && eval "${pcmd/ -o NSMBW / -o $M/bin/NSMBW_$N }")
(cd $G5/export/build && eval "${pcmd/ -o NSMBW / -o $M/bin/NSMBW_${N}_input_test $RT/pc/input_script.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }")
(cd $M/bin && sha256sum nsmbw_$N.nro nsmbw_$N.elf nsmbw_${N}_input_test.nro NSMBW_$N NSMBW_${N}_input_test | tee $M/runs/bin_sha256_$N.txt)
