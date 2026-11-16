#!/usr/bin/env bash
# [r43a] build_pgo_m6.sh <tag> [src=r41m/nWiiRuntime]: NRO de Switch = objetos PGO de m5 (r17pgo/nx/m5_pgo: juego con
# PGO, runtime SIN perfil) + un runtime cualquiera. Mezcla de r40d/tools/build_pgo_d.sh y r38c/tools/nx_c38_remote.sh:
# se recompilan con LA MISMA orden de ninja que m5_pgo (a) las fuentes de src/ que difieren de r17pgo/m5src y (b) las
# que incluyen una cabecera de src/ que difiere; luego ar, enlace, nacptool, elf2nro.
# Corre en ESTA laptop; compila en la otra (ssh nsmbw-build) en ~/nsmbw-port/r9/r43a/nx/<tag> (copia --reflink de
# r17pgo/nx/m5_pgo; no toca r17pgo). Resultado: r9/r43a/bin/<tag>_pgo.nro/.elf (+ .sha256).
# Comprobación de la herramienta: con SRC = fuentes de v37a / c1 / d2 debe salir el NRO de su frente byte a byte.
set -euo pipefail
TAG=${1:?tag}
L=/home/djvemo/nsmbw-port/r9/r43a; SRC=$(realpath "${2:-$L/nWiiRuntime}")
R=/home/djvemo/nsmbw-port/r9/r43a          # misma ruta en la otra laptop
ssh nsmbw-build "mkdir -p $R/src_$TAG $R/nx $R/gen_$TAG"
rsync -a --delete "$SRC/" "nsmbw-build:$R/src_$TAG/"
# [r43a] GEN24=1: tambien output_24.cpp propio (gen/output_24.cpp, kernel 4 de la reverb), con la orden PGO de m5
[[ -n ${GEN24:-} ]] && rsync -a "$L/gen/output_24.cpp" "nsmbw-build:$R/gen_$TAG/output_24.cpp"
ssh nsmbw-build /home/djvemo/nsmbw-port/scripts/lim bash -s -- "$TAG" "${GEN24:-}" <<'EOF'
set -euo pipefail
export LC_ALL=C
TAG=$1; R=/home/djvemo/nsmbw-port/r9/r43a; P5=/home/djvemo/nsmbw-port/r9/r17pgo
B=$R/nx/$TAG; S=$R/src_$TAG
diff -rq $P5/m5src/nWiiRuntime/include "$S/include" > /dev/null || { echo "build_pgo_m6: include/ distinto de m5src (no soportado)"; exit 2; }
CH=$(cd "$S" && { diff -rq $P5/m5src/nWiiRuntime/src src || true; } | sed -n 's#^Files .* and src/\(.*\) differ$#\1#p')
[[ -z $(cd "$S" && { diff -rq $P5/m5src/nWiiRuntime/src src || true; } | grep -v '^Files .* differ$') ]] || { echo "build_pgo_m6: archivos nuevos/ausentes"; exit 2; }
HDR=$(tr ' ' '\n' <<<"$CH" | grep -E '\.(h|inc)$' || true)
RE=$( { tr ' ' '\n' <<<"$CH" | grep -E '\.cpp$' || true
        for h in $HDR; do (cd "$S/src" && grep -rlE "#include \"([^\"]*/)?$(basename $h)\"" . | sed 's#^\./##' | grep '\.cpp$') || true; done; } | sort -u)
echo "cambian: $(echo $CH)"; echo "recompilar: $(echo $RE)"
rm -rf "$B"; cp -a --reflink=auto $P5/nx/m5_pgo "$B"
(cd $P5/nx/m5_pgo && ninja -t commands nsmbw.nro) > "$B/m5_cmds.txt"
sed -e "s#$P5/nx/m5_pgo#$B#g" "$B/m5_cmds.txt" > "$B/cmds.txt"
: > "$B/run.txt"; n=0
while IFS= read -r line; do
  src=$(grep -oE -- '-c [^ ]+' <<<"$line" | cut -c4- || true)
  rel=$(sed -nE 's#.*/(m4src|m5src)/nWiiRuntime/src/##p' <<<"$src")
  [[ -n $rel ]] && grep -qxF "$rel" <<<"$RE" || continue
  sed -E "s#$P5/(m4src|m5src)/nWiiRuntime/src/$rel#$S/src/$rel#" <<<"$line" >> "$B/run.txt"; n=$((n + 1))
done < "$B/cmds.txt"
[[ $n == $(grep -c . <<<"$RE") ]] || { echo "build_pgo_m6: $n ordenes de compilacion para $(grep -c . <<<"$RE") fuentes"; exit 2; }
G24=0
if [[ -n ${2:-} ]]; then  # [r43a] output_24.cpp propio: misma orden, otra fuente; el #line de su cabeza conserva el nombre
  grep -E -- "-c $P5/m4src/export/output_24\.cpp\$" "$B/cmds.txt" | sed -E "s#-c $P5/m4src/export/output_24\.cpp\$#-Wno-error=coverage-mismatch -c $R/gen_$TAG/output_24.cpp#" >> "$B/run.txt"
  G24=1; grep -q "gen_$TAG/output_24.cpp" "$B/run.txt" || { echo "build_pgo: sin orden de output_24"; exit 2; }
fi
grep -E 'gcc-ar qc runtime/nwiiruntime/libnwiiruntime.a|-o nsmbw\.elf|nacptool|elf2nro' "$B/cmds.txt" >> "$B/run.txt"
[[ $(wc -l < "$B/run.txt") == $((n + G24 + 4)) ]] || { echo "build_pgo_m6: faltan ordenes finales"; exit 2; }
! grep -q "m5_pgo" "$B/run.txt" || { echo "build_pgo_m6: quedan rutas de m5_pgo"; exit 2; }
export DEVKITPRO=/opt/devkitpro
cd "$B"; rm -f nsmbw.elf nsmbw.nro
while IFS= read -r line; do nice -n 5 bash -c "$line" >> run.log 2>&1 || { tail -30 run.log; exit 3; }; done < run.txt
[[ -s nsmbw.elf && -s nsmbw.nro ]] || { echo "build_pgo_m6: sin elf/nro"; exit 3; }
python3 $P5/tools/check_fm_tab_g5.py nsmbw.elf 2>&1 | tail -2
grep -ci 'warning' run.log || true
EOF
mkdir -p $L/bin
rsync -a "nsmbw-build:$R/nx/$TAG/nsmbw.nro" "$L/bin/${TAG}_pgo.nro"
rsync -a "nsmbw-build:$R/nx/$TAG/nsmbw.elf" "$L/bin/${TAG}_pgo.elf"
(cd $L/bin && sha256sum ${TAG}_pgo.nro ${TAG}_pgo.elf | tee $L/bin/${TAG}_pgo.sha256)
