#!/usr/bin/env bash
# build_m3.sh   (correr con ~/nsmbw-port/scripts/lim, JOBS<=2)
# r29m3: m3 = m2 (r9/r24m2: runtime r7f + código generado G6 e2) + frente X1 (r9/r26x1) + frente D1 (r9/r28d1).
#   1) runtime (nWiiRuntime/ = r7f + x1_runtime_vs_r7f.patch + d1_runtime_vs_r22r7.patch, ver m3_runtime_vs_r7f.patch):
#      libnwiiruntime.a + main.o + libglad.a (Switch, nx/build) y libnwiiruntime.a + main.cpp.o (PC, pc_build),
#      con la misma configuración cmake que r22r7/r26x1/r28d1 (toolchain y export de r14g5)
#   2) los output_*.cpp de export/ (= copia de r20g6/e2/export + tools/apply_x1.py + tools/apply_d1.py) que difieren
#      de e2, con el comando de compilación exacto de r20g6/e2/{nx_obj,pc_obj}/compile.cmd (solo cambian el export
#      y el directorio de cabeceras del runtime, cuyas cabeceras del código generado son idénticas: se comprueba)
#   3) comprobación: los miembros del libnwiiruntime.a nuevo que no son de X1/D1 deben ser idénticos a los de r7f (rt/ de m2)
#   4) enlace: la línea de m2 (r24m2/nx/link_m2.cmd, pc_link_m2.cmd) con las piezas nuevas; orden L = el de m2 +
#      las secciones del worker de R7 tras PrepFrame (tools/mk_order.py -> nx/order/m3.txt)
set -euo pipefail
M=/home/djvemo/nsmbw-port/r9/r29m3; M2=/home/djvemo/nsmbw-port/r9/r24m2; G=/home/djvemo/nsmbw-port/r9/r20g6
G5=/home/djvemo/nsmbw-port/r9/r14g5; P=/home/djvemo/nsmbw-port
export PATH="$HOME/.local/bin:$PATH" DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
DKP=$DEVKITPRO; ICON=$DKP/libnx/default_icon.jpg; N=${N:-m3}; J=${JOBS:-2}
(( J <= 2 )) || { echo "JOBS<=2" >&2; exit 1; }
mkdir -p "$M/bin" "$M/runs" "$M/nx/$N/game" "$M/nx/$N/test" "$M/nx/check"

for h in cpu_context.h config.h nwii_env.h event_scheduler.h hw/hw.h; do
  cmp -s "$M/nWiiRuntime/include/runtime/$h" "$G5/nWiiRuntime/include/runtime/$h" || { echo "m3: $h difiere de r14g5" >&2; exit 2; }
done

# ---- 1) runtime ----
B=$M/nx/build
if [[ ! -f $B/build.ninja ]]; then
  cmake -S $P/switch/nsmbw -B "$B" -G Ninja -DCMAKE_TOOLCHAIN_FILE=$G5/cmake/nsmbw-switch.cmake \
    -DCMAKE_BUILD_TYPE=Release -DNWII_RUNTIME_DIR="$M/nWiiRuntime" -DNSMBW_EXPORT_DIR="$G5/export" \
    -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$P/switch/nsmbw/build/_deps/tomlplusplus-src \
    -DFETCHCONTENT_SOURCE_DIR_GLAD=$P/switch/nsmbw/build/_deps/glad-src \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache > "$M/configure_nx.log"
fi
GLAD=$(cd "$B" && ninja -t targets all | grep -o '^[^:]*libglad\.a' | head -1)
MAINO=$(cd "$B" && ninja -t targets all | grep -o '^CMakeFiles/nsmbw.dir/[^:]*src/core/main.o' | head -1)
nice -n 5 ninja -C "$B" -j"$J" runtime/nwiiruntime/libnwiiruntime.a "$GLAD" "$MAINO"
NXLIB=$B/runtime/nwiiruntime/libnwiiruntime.a; NXMAIN=$B/$MAINO; NXGLAD=$B/$GLAD

PJ=$M/pcproj PB=$M/pc_build DEPS=$P/recomp/export/build/_deps
mkdir -p "$PJ"
cp -f "$G5/export/CMakeLists.txt" "$PJ/"
for f in "$G5"/export/output_*.cpp "$G5/export/main_output.cpp" "$G5/export/fastdispatch.cpp" "$G5/export/functions.h"; do ln -sf "$f" "$PJ/"; done
ln -sfn "$M/nWiiRuntime" "$PJ/nWiiRuntime"
if [[ ! -f $PB/build.ninja ]]; then
  cmake -S "$PJ" -B "$PB" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=mold \
    -DFETCHCONTENT_SOURCE_DIR_SDL2=$DEPS/sdl2-src -DFETCHCONTENT_SOURCE_DIR_TOMLPLUSPLUS=$DEPS/tomlplusplus-src \
    -DFETCHCONTENT_SOURCE_DIR_GLAD=$DEPS/glad-src \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DSDL_PIPEWIRE=OFF \
    -DCMAKE_C_FLAGS="-Wno-error=incompatible-pointer-types" > "$M/configure_pc.log"
fi
nice -n 5 ninja -C "$PB" -j"$J" nWiiRuntime/libnwiiruntime.a CMakeFiles/NSMBW.dir/nWiiRuntime/src/core/main.cpp.o
PCLIB=$PB/nWiiRuntime/libnwiiruntime.a; PCMAIN=$PB/CMakeFiles/NSMBW.dir/nWiiRuntime/src/core/main.cpp.o

# ---- 2) output_*.cpp con ganchos ----
MODS=()
for f in "$M"/export/output_*.cpp; do b=$(basename "$f" .cpp); cmp -s "$f" "$G/e2/export/$b.cpp" || MODS+=("$b"); done
echo "[m3] output_*.cpp cambiados: ${MODS[*]}"; echo "${MODS[*]}" > "$M/runs/mods.txt"
for W in nx pc; do
  O=$M/${W}_obj; mkdir -p "$O"
  t=$(cat $G/e2/${W}_obj/compile.cmd)
  t=${t//-I$G\/e2\/export/-I$M\/export}
  if [[ $W == nx ]]; then t=${t//-I$G5\/nWiiRuntime\//-I$M\/nWiiRuntime\/}; RUNDIR=$G5/nx/build
  else t=${t//-I$G5\/export\/nWiiRuntime\//-I$M\/nWiiRuntime\/}; RUNDIR=$G5/export/build; fi
  [[ $t != *r20g6* && $t != *r14g5/nWiiRuntime* && $t != *r14g5/export/nWiiRuntime* ]] || { echo "m3: plantilla $W mal: $t" >&2; exit 3; }
  echo "$t" > "$O/compile.cmd"
  { echo "rule cc"; echo "  command = cd $RUNDIR && nice -n 5 $t"; echo "  description = $W \$in"
    for b in "${MODS[@]}"; do echo "build $O/$b.o: cc $M/export/$b.cpp"; done; } > "$O/build.ninja"
  ninja -f "$O/build.ninja" -j"$J"
done

# ---- 3) el runtime nuevo = r7f salvo X1/D1 ----
cmp_ar() {  # cmp_ar <lib nuevo> <lib r7f> <salida>
  local d; d=$(mktemp -d "$M/nx/check/ar.XXXX"); mkdir -p "$d/a" "$d/b"
  (cd "$d/a" && ar x "$1"); (cd "$d/b" && ar x "$2")
  { for f in $(ls "$d/a" "$d/b" | grep -v ':$' | sort -u); do
      if [[ ! -f $d/b/$f ]]; then echo "nuevo  $f"; elif [[ ! -f $d/a/$f ]]; then echo "FALTA  $f";
      elif cmp -s "$d/a/$f" "$d/b/$f"; then echo "igual  $f"; else echo "DIFIERE $f"; fi
    done; } > "$3"; rm -rf "$d"
  echo "[m3] $(basename "$3"): $(grep -c '^igual' "$3") iguales; distintos/nuevos: $(grep -v '^igual' "$3" | awk '{print $2}' | tr '\n' ' ')"
}
cmp_ar "$NXLIB" "$M2/rt/nx/libnwiiruntime.a" "$M/nx/check/lib_nx_vs_r7f.txt"
cmp_ar "$PCLIB" "$M2/rt/pc/libnwiiruntime.a" "$M/nx/check/lib_pc_vs_r7f.txt"
cmp -s "$NXMAIN" "$M2/rt/nx/main.o" && echo "[m3] main.o (Switch) = r7f" || echo "[m3] AVISO: main.o (Switch) distinto de r7f"
cmp -s "$NXGLAD" "$M2/rt/nx/libglad.a" && echo "[m3] libglad.a = r7f" || echo "[m3] AVISO: libglad.a distinta de r7f"
cmp -s "$PCMAIN" "$M2/rt/pc/main.cpp.o" && echo "[m3] main.cpp.o (PC) = r7f" || echo "[m3] AVISO: main.cpp.o (PC) distinto de r7f"

# ---- 4) enlace ----
python3 "$M/tools/mk_order.py" "$M2/nx/order/g6e2.txt" "$M2/nx/ref/r7f_dk3drel.txt" "$M/nx/order/$N.txt"
cmd=$(cat $M2/nx/link_m2.cmd)
cmd=${cmd/$M2\/rt\/nx\/main.o/$NXMAIN}
cmd=${cmd/$M2\/rt\/nx\/libnwiiruntime.a/$NXLIB}
cmd=${cmd/$M2\/rt\/nx\/libglad.a/$NXGLAD}
for b in "${MODS[@]}"; do
  [[ $cmd == *"$G/e2/nx_obj/$b.o"* ]] || { echo "m3: $b no está en la línea de m2" >&2; exit 4; }
  cmd=${cmd/$G\/e2\/nx_obj\/$b.o/$M/nx_obj/$b.o}
done
[[ $cmd != *r24m2* ]] || { echo "m3: queda algo de r24m2 en el enlace" >&2; exit 4; }
echo "$cmd" > "$M/nx/link_$N.cmd"
O=$M/nx/order/$N.txt D=$M/nx/$N
(cd $G5/nx/build && eval "${cmd/-o nsmbw.elf/-o $D/game/nsmbw.elf -Wl,--section-ordering-file=$O -Wl,-Map=$D/game/nsmbw.map}")
$DKP/tools/bin/nacptool --create "${NACP_NAME:-NSMBW PRUEBA m3 (m2 + X1 + D1)}" "nsmbw-port" "$N" "$D/game/nsmbw.nacp" > /dev/null
$DKP/tools/bin/elf2nro "$D/game/nsmbw.elf" "$M/bin/nsmbw_$N.nro" --icon=$ICON --nacp="$D/game/nsmbw.nacp" > /dev/null
cp -f "$D/game/nsmbw.elf" "$M/bin/nsmbw_$N.elf"
tcmd=${cmd/-o nsmbw.elf/-o $D/test/nsmbw.elf -Wl,--section-ordering-file=$O}
tcmd=${tcmd/ -fPIE / -fPIE $M2/rt/nx/input_script_nx.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }
(cd $G5/nx/build && eval "$tcmd")
$DKP/tools/bin/elf2nro "$D/test/nsmbw.elf" "$M/bin/nsmbw_${N}_input_test.nro" --icon=$ICON --nacp="$D/game/nsmbw.nacp" > /dev/null

pcmd=$(cat $M2/nx/pc_link_m2.cmd)
pcmd=${pcmd/$M2\/rt\/pc\/main.cpp.o/$PCMAIN}
pcmd=${pcmd/$M2\/rt\/pc\/libnwiiruntime.a/$PCLIB}
for b in "${MODS[@]}"; do
  [[ $pcmd == *"$G/e2/pc_obj/$b.o"* ]] || { echo "m3: $b no está en la línea de PC de m2" >&2; exit 4; }
  pcmd=${pcmd/$G\/e2\/pc_obj\/$b.o/$M/pc_obj/$b.o}
done
[[ $pcmd != *r24m2* ]] || { echo "m3: queda algo de r24m2 en el enlace de PC" >&2; exit 4; }
echo "$pcmd" > "$M/nx/pc_link_$N.cmd"
(cd $G5/export/build && eval "${pcmd/ -o NSMBW / -o $M/bin/NSMBW_$N }")
(cd $G5/export/build && eval "${pcmd/ -o NSMBW / -o $M/bin/NSMBW_${N}_input_test $M2/rt/pc/input_script.o -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }")
(cd $M/bin && sha256sum nsmbw_$N.nro nsmbw_$N.elf nsmbw_${N}_input_test.nro NSMBW_$N NSMBW_${N}_input_test | tee $M/runs/bin_sha256_$N.txt)
