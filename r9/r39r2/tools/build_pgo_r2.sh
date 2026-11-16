#!/usr/bin/env bash
# build_pgo_r2.sh <tag>: NRO de Switch = m5 PGO (r17pgo/nx/m5_pgo, objetos del juego con PGO, runtime sin perfil) con
# los archivos del runtime de r39r2 que difieren de m5src recompilados con LA MISMA orden que m5_pgo.
# Se ejecuta en ESTA laptop; compila en la otra (ssh nsmbw-build) en ~/nsmbw-port/r9/r39r2/nx/<tag> (copia --reflink de
# r17pgo/nx/m5_pgo; no toca r17pgo). Resultado: r9/r39r2/bin/r2_<tag>.nro/.elf (+ sha256).
set -euo pipefail
TAG=${1:?tag}
L=/home/djvemo/nsmbw-port/r9/r39r2; SRC=${SRC:-$L/nWiiRuntime}
R=/home/djvemo/nsmbw-port/r9/r39r2          # misma ruta en la otra laptop
P5=/home/djvemo/nsmbw-port/r9/r17pgo
FILES=(src/hle/gx.cpp src/hle/gx/renderer_gl.cpp src/hle/texture_cache_gl.cpp src/hle/gx/tev_shader_gen.cpp)
ssh nsmbw-build "mkdir -p $R/src_$TAG $R/nx"
rsync -a --delete "$SRC/" "nsmbw-build:$R/src_$TAG/"   # arbol entero (includes relativos); solo se compilan FILES
ssh nsmbw-build bash -s -- "$TAG" <<'EOF'
set -euo pipefail
TAG=$1; R=/home/djvemo/nsmbw-port/r9/r39r2; P5=/home/djvemo/nsmbw-port/r9/r17pgo
B=$R/nx/$TAG
rm -rf "$B"; cp -a --reflink=auto $P5/nx/m5_pgo "$B"
cd "$P5/nx/m5_pgo"
ninja -t commands nsmbw.nro > "$B/m5_cmds.txt"
cd "$B"
sub() { sed -e "s#$P5/nx/m5_pgo#$B#g" -e "s#$P5/m[45]src/nWiiRuntime/src/hle/gx.cpp#$R/src_$TAG/src/hle/gx.cpp#g" \
  -e "s#$P5/m[45]src/nWiiRuntime/src/hle/gx/renderer_gl.cpp#$R/src_$TAG/src/hle/gx/renderer_gl.cpp#g" \
  -e "s#$P5/m[45]src/nWiiRuntime/src/hle/texture_cache_gl.cpp#$R/src_$TAG/src/hle/texture_cache_gl.cpp#g" \
  -e "s#$P5/m[45]src/nWiiRuntime/src/hle/gx/tev_shader_gen.cpp#$R/src_$TAG/src/hle/gx/tev_shader_gen.cpp#g"; }
sub < m5_cmds.txt > cmds.txt
grep -c "src_$TAG" cmds.txt || true
{ grep -E "src_$TAG/src/hle/(gx|gx/renderer_gl|texture_cache_gl|gx/tev_shader_gen)\.cpp" cmds.txt
  grep -E "gcc-ar qc runtime/nwiiruntime/libnwiiruntime.a" cmds.txt
  grep -E "^: && .*-o nsmbw.elf|-o nsmbw\.elf" cmds.txt | head -1
  grep -E "nacptool|elf2nro" cmds.txt; } > run.txt
wc -l run.txt
[[ $(wc -l < run.txt) == 8 ]] || { echo "build_pgo_r2: se esperaban 8 ordenes"; exit 2; }
export DEVKITPRO=/opt/devkitpro
rm -f nsmbw.elf nsmbw.nro
while IFS= read -r line; do nice -n 5 bash -c "$line" >> run.log 2>&1 || { tail -30 run.log; exit 3; }; done < run.txt
[[ -s nsmbw.elf && -s nsmbw.nro ]] || { echo "build_pgo_r2: sin elf/nro"; exit 3; }
ls -la nsmbw.nro nsmbw.elf
EOF
rsync -a "nsmbw-build:$R/nx/$TAG/nsmbw.nro" "$L/bin/r2_$TAG.nro"
rsync -a "nsmbw-build:$R/nx/$TAG/nsmbw.elf" "$L/bin/r2_$TAG.elf"
(cd $L/bin && sha256sum r2_$TAG.nro r2_$TAG.elf | tee $L/bin/r2_$TAG.sha256)
