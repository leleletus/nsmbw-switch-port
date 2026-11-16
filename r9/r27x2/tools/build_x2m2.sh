#!/usr/bin/env bash
# build_x2m2.sh   (correr con ~/nsmbw-port/scripts/lim; el nombre build_*.sh toma turno de compilación)
# X2 sobre la troncal m2 (r9/r24m2 = runtime r7f + código G6 e2), sin tocar r24m2 ni r20g6:
#   1) tools/apply_x2.py sobre r20g6/e2/export -> m2/export (solo los output_N con sitios MMIO de escritura)
#   2) compila esos archivos con las órdenes de e2 (nx_obj/compile.cmd, pc_obj/compile.cmd) + -I del runtime de
#      r27x2 al final (solo para x2_wgpipe.h; las demás cabeceras siguen siendo las de r14g5, idénticas)
#   3) enlaza las líneas de m2 (r24m2/nx/link_m2.cmd, pc_link_m2.cmd) cambiando esos objetos y el runtime de rt/
#      (r7f) por el de x2b (nx/build, pc_build de r27x2), con el orden L de m2
#   -> bin/nsmbw_x2m2.nro/.elf (+ _input_test.nro, solo pruebas), bin/NSMBW_x2m2(_input_test)
set -euo pipefail
X=/home/djvemo/nsmbw-port/r9/r27x2; M2=/home/djvemo/nsmbw-port/r9/r24m2; G=/home/djvemo/nsmbw-port/r9/r20g6/e2
G5=/home/djvemo/nsmbw-port/r9/r14g5; DKP=/opt/devkitpro; ICON=$DKP/libnx/default_icon.jpg
W=$X/m2; mkdir -p $W/export $W/nx_obj $W/pc_obj $W/nx/game $W/nx/test $X/bin
FILES=$(for f in $G/export/output_*.cpp; do grep -qE 'mv_\.write(8|16|32|_f32)_io\(|psq_st_io\(' $f && basename $f .cpp || true; done | tr '\n' ' ')
echo "[x2m2] archivos: $FILES"
if [[ ! -f $W/export/.done ]]; then
  rm -f $W/export/output_*.cpp $W/export/x2_sha256.txt
  python3 -I $X/tools/apply_x2.py $G/export $W/export $FILES
  ln -f $G/export/functions.h $W/export/functions.h
  touch $W/export/.done
fi
NXC=$(cat $G/nx_obj/compile.cmd); PCC=$(cat $G/pc_obj/compile.cmd)
for f in $FILES; do
  if [[ ! -f $W/nx_obj/$f.o || $W/export/$f.cpp -nt $W/nx_obj/$f.o ]]; then
    c=${NXC//\$out/$W/nx_obj/$f.o}; c=${c//\$in/$W/export/$f.cpp}
    echo "[x2m2] nx $f"; (cd $G5/nx/build && eval "nice -n 5 $c -I$X/nWiiRuntime/include")
  fi
  if [[ ! -f $W/pc_obj/$f.o || $W/export/$f.cpp -nt $W/pc_obj/$f.o ]]; then
    c=${PCC//\$out/$W/pc_obj/$f.o}; c=${c//\$in/$W/export/$f.cpp}
    echo "[x2m2] pc $f"; (cd $G5/export/build && eval "nice -n 5 $c -I$X/nWiiRuntime/include")
  fi
done
# ---- Switch ----
NXB=$X/nx/build
cmd=$(cat $M2/nx/link_m2.cmd)
cmd=${cmd/$M2\/rt\/nx\/main.o/$NXB/CMakeFiles/nsmbw.dir/home/djvemo/nsmbw-port/r9/r27x2/nWiiRuntime/src/core/main.o}
cmd=${cmd/$M2\/rt\/nx\/libnwiiruntime.a/$NXB/runtime/nwiiruntime/libnwiiruntime.a}
cmd=${cmd/$M2\/rt\/nx\/libglad.a/$NXB/_deps/glad-build/libglad.a}
for f in $FILES; do
  [[ $cmd == *"$G/nx_obj/$f.o "* ]] || { echo "x2m2: $f.o no está en la línea de m2" >&2; exit 3; }
  cmd=${cmd/$G\/nx_obj\/$f.o /$W/nx_obj/$f.o }
done
[[ $cmd != *"$M2/rt/"* ]] || { echo "x2m2: queda rt/ de m2 en la línea" >&2; exit 3; }
echo "$cmd" > $W/link_x2m2.cmd
O=$M2/nx/order/g6e2.txt
(cd $G5/nx/build && eval "${cmd/-o nsmbw.elf/-o $W/nx/game/nsmbw.elf -Wl,--section-ordering-file=$O}")
$DKP/tools/bin/nacptool --create "NSMBW PRUEBA x2m2 (m2 + X2)" "nsmbw-port" "x2m2" "$W/nx/game/nsmbw.nacp" > /dev/null
$DKP/tools/bin/elf2nro "$W/nx/game/nsmbw.elf" "$X/bin/nsmbw_x2m2.nro" --icon=$ICON --nacp="$W/nx/game/nsmbw.nacp" > /dev/null
cp -f "$W/nx/game/nsmbw.elf" "$X/bin/nsmbw_x2m2.elf"
tcmd=${cmd/-o nsmbw.elf/-o $W/nx/test/nsmbw.elf -Wl,--section-ordering-file=$O}
tcmd=${tcmd/ -fPIE / -fPIE $NXB/input_script_nx.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }
(cd $G5/nx/build && eval "$tcmd")
$DKP/tools/bin/elf2nro "$W/nx/test/nsmbw.elf" "$X/bin/nsmbw_x2m2_input_test.nro" --icon=$ICON --nacp="$W/nx/game/nsmbw.nacp" > /dev/null
# ---- PC ----
PCB=$X/pc_build
pr=$(cat $M2/nx/pc_link_m2.cmd)
pr=${pr/$M2\/rt\/pc\/main.cpp.o/$PCB/CMakeFiles/NSMBW.dir/nWiiRuntime/src/core/main.cpp.o}
pr=${pr/$M2\/rt\/pc\/libnwiiruntime.a/$PCB/nWiiRuntime/libnwiiruntime.a}
for f in $FILES; do
  [[ $pr == *"$G/pc_obj/$f.o "* ]] || { echo "x2m2: $f.o no está en la línea PC de m2" >&2; exit 3; }
  pr=${pr/$G\/pc_obj\/$f.o /$W/pc_obj/$f.o }
done
[[ $pr != *"$M2/rt/"* ]] || { echo "x2m2: queda rt/ de m2 en la línea PC" >&2; exit 3; }
echo "$pr" > $W/pc_link_x2m2.cmd
oo=$(grep -o ' -o [^ ]*' <<<"$pr" | head -1)
(cd $G5/export/build && eval "${pr/$oo/ -o $X/bin/NSMBW_x2m2}")
(cd $G5/export/build && eval "${pr/$oo/ -o $X/bin/NSMBW_x2m2_input_test $PCB/input_script.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx}")
(cd $X/bin && sha256sum nsmbw_x2m2.nro nsmbw_x2m2.elf nsmbw_x2m2_input_test.nro NSMBW_x2m2 NSMBW_x2m2_input_test | tee $X/runs/bin_sha256_x2m2.txt)
