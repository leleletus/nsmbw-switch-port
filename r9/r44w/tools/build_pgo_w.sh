#!/usr/bin/env bash
# [r44w] build_pgo_w.sh <tag> [src=r44w/nWiiRuntime] = r41m/tools/build_pgo_m6.sh + el renderizador nativo del agua
# (w_agua/w_agua_gen.cpp, compilado con la orden de ninja de output_1.o de m5_pgo SIN las opciones de perfil, y
# añadido al enlace). Lo de abajo es el comentario original:
# [r41m] build_pgo_m6.sh <tag> [src=r41m/nWiiRuntime]: NRO de Switch = objetos PGO de m5 (r17pgo/nx/m5_pgo: juego con
# PGO, runtime SIN perfil) + un runtime cualquiera. Mezcla de r40d/tools/build_pgo_d.sh y r38c/tools/nx_c38_remote.sh:
# se recompilan con LA MISMA orden de ninja que m5_pgo (a) las fuentes de src/ que difieren de r17pgo/m5src y (b) las
# que incluyen una cabecera de src/ que difiere; luego ar, enlace, nacptool, elf2nro.
# Corre en ESTA laptop; compila en la otra (ssh nsmbw-build) en ~/nsmbw-port/r9/r41m/nx/<tag> (copia --reflink de
# r17pgo/nx/m5_pgo; no toca r17pgo). Resultado: r9/r41m/bin/<tag>_pgo.nro/.elf (+ .sha256).
# Comprobación de la herramienta: con SRC = fuentes de v37a / c1 / d2 debe salir el NRO de su frente byte a byte.
set -euo pipefail
TAG=${1:?tag}
L=/home/djvemo/nsmbw-port/r9/r44w; SRC=$(realpath "${2:-$L/nWiiRuntime}")
R=/home/djvemo/nsmbw-port/r9/r44w          # misma ruta en la otra laptop
ssh nsmbw-build "mkdir -p $R/src_$TAG $R/nx"
rsync -a --delete "$SRC/" "nsmbw-build:$R/src_$TAG/"
python3 -I $L/tools/gen_w_agua.py /home/djvemo/nsmbw-port/r9/r35m4/export $L/w_agua/w_agua_gen.cpp
$L/tools/gen_w_sim_all.sh /home/djvemo/nsmbw-port/r9/r35m4/export $L/w_agua/w_sim_gen.inc
ssh nsmbw-build "mkdir -p $R/wagua_$TAG"
rsync -a --delete "$L/w_agua/w_agua_gen.cpp" "$L/w_agua/w_agua_cola.h" "$L/w_agua/w_agua_hook.inc" "$L/w_agua/w_sim_gen.inc" "$L/w_agua/w_sim_fpr.h" "nsmbw-build:$R/wagua_$TAG/"
ssh nsmbw-build /home/djvemo/nsmbw-port/scripts/lim bash -s -- "$TAG" <<'EOF'
set -euo pipefail
export LC_ALL=C
TAG=$1; R=/home/djvemo/nsmbw-port/r9/r44w; P5=/home/djvemo/nsmbw-port/r9/r17pgo
B=$R/nx/$TAG; S=$R/src_$TAG
# include/: las cabeceras cambiadas (y las de include/ que las incluyen, un nivel) cuentan como cabeceras cambiadas;
# sus includers de src/ se recompilan con -I del include de src (m6f: fifo_parser.h de D, como build_pgo_m6d.sh de r40d).
IDIFF=$(diff -rq $P5/m5src/nWiiRuntime/include "$S/include" || true)
[[ -z $(grep -v '^Files .* differ$' <<<"$IDIFF" | grep .) ]] || { echo "build_pgo_m6: include/ con archivos nuevos/ausentes: $IDIFF"; exit 2; }
IHDR=$(sed -n 's#^Files .* and .*/include/\(.*\) differ$#\1#p' <<<"$IDIFF")
for h in $IHDR; do IHDR="$IHDR $(cd "$S/include" && grep -rlE "#include [<\"]([^\">]*/)?$(basename $h)[\">]" . | sed 's#^\./##' || true)"; done
echo "include cambiado: $(echo $IHDR)"
CH=$(cd "$S" && { diff -rq $P5/m5src/nWiiRuntime/src src || true; } | sed -n 's#^Files .* and src/\(.*\) differ$#\1#p')
[[ -z $(cd "$S" && { diff -rq $P5/m5src/nWiiRuntime/src src || true; } | grep -v '^Files .* differ$') ]] || { echo "build_pgo_m6: archivos nuevos/ausentes"; exit 2; }
HDR=$(tr ' ' '\n' <<<"$CH" | grep -E '\.(h|inc)$' || true)
RE=$( { tr ' ' '\n' <<<"$CH" | grep -E '\.cpp$' || true
        for h in $HDR $IHDR; do (cd "$S/src" && grep -rlE "#include [<\"]([^\">]*/)?$(basename $h)[\">]" . | sed 's#^\./##' | grep '\.cpp$') || true; done; } | sort -u)
echo "cambian: $(echo $CH)"; echo "recompilar: $(echo $RE)"
rm -rf "$B"; cp -a --reflink=auto $P5/nx/m5_pgo "$B"
(cd $P5/nx/m5_pgo && ninja -t commands nsmbw.nro) > "$B/m5_cmds.txt"
sed -e "s#$P5/nx/m5_pgo#$B#g" "$B/m5_cmds.txt" > "$B/cmds.txt"
: > "$B/run.txt"; n=0
while IFS= read -r line; do
  src=$(grep -oE -- '-c [^ ]+' <<<"$line" | cut -c4- || true)
  rel=$(sed -nE 's#.*/(m4src|m5src)/nWiiRuntime/src/##p' <<<"$src")
  [[ -n $rel ]] && grep -qxF "$rel" <<<"$RE" || continue
  if [[ -n ${IHDR// /} ]]; then
    sed -E -e "s#$P5/(m4src|m5src)/nWiiRuntime/src/$rel#$S/src/$rel#" -e "s#-I$P5/(m4src|m5src)/nWiiRuntime/include#-I$S/include#g" <<<"$line" >> "$B/run.txt"
  else
    sed -E "s#$P5/(m4src|m5src)/nWiiRuntime/src/$rel#$S/src/$rel#" <<<"$line" >> "$B/run.txt"
  fi
  n=$((n + 1))
done < "$B/cmds.txt"
[[ $n == $(grep -c . <<<"$RE") ]] || { echo "build_pgo_m6: $n ordenes de compilacion para $(grep -c . <<<"$RE") fuentes"; exit 2; }
# r44w: w_agua_gen.o con la orden de output_1.o (sin perfil ni .d), y al enlace
WS=$R/wagua_$TAG; WO=$B/w_agua_gen.o
c1=$(grep -E -- '-c [^ ]*/export/output_1\.cpp' "$B/cmds.txt")
[[ -n $c1 ]] || { echo "build_pgo_w: sin orden de output_1"; exit 2; }
c1=$(sed -E -e 's# -fprofile-[a-z-]+(=[^ ]*)?##g' -e 's# -Wno-missing-profile##' -e 's# -MD -MT [^ ]+ -MF [^ ]+##' \
       -e "s# -o [^ ]+ -c [^ ]+# -I$WS -o $WO -c $WS/w_agua_gen.cpp#" <<<"$c1")
! grep -q -- '-fprofile' <<<"$c1" || { echo "build_pgo_w: quedan opciones de perfil"; exit 2; }
echo "$c1" >> "$B/run.txt"; n=$((n + 1))
grep -E 'gcc-ar qc runtime/nwiiruntime/libnwiiruntime.a' "$B/cmds.txt" >> "$B/run.txt"
grep -E -- '-o nsmbw\.elf' "$B/cmds.txt" | sed "s# -o nsmbw\.elf # $WO -o nsmbw.elf #" >> "$B/run.txt"
grep -E 'nacptool|elf2nro' "$B/cmds.txt" >> "$B/run.txt"
grep -q "w_agua_gen.o -o nsmbw.elf" "$B/run.txt" || { echo "build_pgo_w: w_agua_gen.o no entra al enlace"; exit 2; }
[[ $(wc -l < "$B/run.txt") == $((n + 4)) ]] || { echo "build_pgo_m6: faltan ordenes finales"; exit 2; }
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
