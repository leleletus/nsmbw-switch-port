#!/usr/bin/env bash
# [r40d] build_pgo_d.sh <tag>: NRO de Switch = m5 PGO (r17pgo/nx/m5_pgo: objetos del juego con PGO, runtime sin perfil)
# + las 6 fuentes del runtime de r40d que difieren de m5src (las 4 de v37a: gx, ios, os_hle_stubs, main; y
# renderer_gl, fifo_parser; las 6 con -I del include de r40d, que solo cambia fifo_parser.h sin tocar la disposicion), recompiladas con LA MISMA orden de ninja que m5_pgo. Copia de r39r2/tools/build_pgo_r2.sh.
# Corre en ESTA laptop; compila en la otra (ssh nsmbw-build) en ~/nsmbw-port/r9/r40d/nx/<tag> (copia --reflink de
# r17pgo/nx/m5_pgo; no toca r17pgo). Resultado: r9/r40d/bin/<tag>_pgo.nro/.elf (+ sha256).
set -euo pipefail
TAG=${1:?tag}
L=/home/djvemo/nsmbw-port/r9/r40d; SRC=${SRC:-$L/nWiiRuntime}
R=/home/djvemo/nsmbw-port/r9/r40d          # misma ruta en la otra laptop
ssh nsmbw-build "mkdir -p $R/src_$TAG $R/nx"
rsync -a --delete "$SRC/" "nsmbw-build:$R/src_$TAG/"
ssh nsmbw-build bash -s -- "$TAG" <<'EOF'
set -euo pipefail
TAG=$1; R=/home/djvemo/nsmbw-port/r9/r40d; P5=/home/djvemo/nsmbw-port/r9/r17pgo
B=$R/nx/$TAG; S=$R/src_$TAG
rm -rf "$B"; cp -a --reflink=auto $P5/nx/m5_pgo "$B"
cd "$P5/nx/m5_pgo"
ninja -t commands nsmbw.nro > "$B/m5_cmds.txt"
cd "$B"
sub() { sed -e "s#$P5/nx/m5_pgo#$B#g" \
  -e "s#$P5/m[45]src/nWiiRuntime/src/hle/gx\.cpp#$S/src/hle/gx.cpp#g" \
  -e "s#$P5/m[45]src/nWiiRuntime/src/hle/gx/renderer_gl\.cpp#$S/src/hle/gx/renderer_gl.cpp#g" \
  -e "s#$P5/m[45]src/nWiiRuntime/src/hle/ios\.cpp#$S/src/hle/ios.cpp#g" \
  -e "s#$P5/m[45]src/nWiiRuntime/src/core/os_hle_stubs\.cpp#$S/src/core/os_hle_stubs.cpp#g" \
  -e "s#$P5/m[45]src/nWiiRuntime/src/core/main\.cpp#$S/src/core/main.cpp#g" \
  -e "s#$P5/m[45]src/nWiiRuntime/src/hle/gx/fifo_parser\.cpp#$S/src/hle/gx/fifo_parser.cpp#g"; }
sub < m5_cmds.txt > cmds.txt
{ grep -E "$S/src/(hle/gx|hle/gx/renderer_gl|hle/gx/fifo_parser|hle/ios|core/os_hle_stubs|core/main)\.cpp" cmds.txt | sed "s#-I$P5/m[45]src/nWiiRuntime/include#-I$S/include#g"
  grep -E "gcc-ar qc runtime/nwiiruntime/libnwiiruntime.a" cmds.txt
  grep -E "^: && .*-o nsmbw.elf|-o nsmbw\.elf" cmds.txt | head -1
  grep -E "nacptool|elf2nro" cmds.txt; } > run.txt
wc -l run.txt
[[ $(wc -l < run.txt) == 10 ]] || { echo "build_pgo_d: se esperaban 10 ordenes"; cat run.txt | cut -c1-200; exit 2; }
grep -q "m5_pgo" run.txt && { echo "build_pgo_d: quedan rutas de m5_pgo"; exit 2; }
export DEVKITPRO=/opt/devkitpro
rm -f nsmbw.elf nsmbw.nro
while IFS= read -r line; do nice -n 5 bash -c "$line" >> run.log 2>&1 || { tail -30 run.log; exit 3; }; done < run.txt
[[ -s nsmbw.elf && -s nsmbw.nro ]] || { echo "build_pgo_d: sin elf/nro"; exit 3; }
python3 $P5/tools/check_fm_tab_g5.py nsmbw.elf 2>&1 | tail -2
ls -la nsmbw.nro nsmbw.elf
EOF
rsync -a "nsmbw-build:$R/nx/$TAG/nsmbw.nro" "$L/bin/${TAG}_pgo.nro"
rsync -a "nsmbw-build:$R/nx/$TAG/nsmbw.elf" "$L/bin/${TAG}_pgo.elf"
(cd $L/bin && sha256sum ${TAG}_pgo.nro ${TAG}_pgo.elf | tee $L/bin/${TAG}_pgo.sha256)
