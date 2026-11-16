#!/usr/bin/env bash
# nx_c38_remote.sh <nombre> (en la OTRA laptop, con ~/nsmbw-port/scripts/lim): NRO PGO = objetos PGO de m5
# (r17pgo/nx/m5_pgo, runtime SIN perfil) + runtime de r38c. No toca m5src ni nx/m5_pgo:
#   r17pgo/c38src_<nombre>/nWiiRuntime = copia de m5src + las fuentes de r9/r38c/nWiiRuntime (traidas con push)
#   r17pgo/nx/c38_<nombre>             = copia de nx/m5_pgo; se recompilan SOLO las fuentes que cambian o incluyen
#                                         s2.h / switch_platform.h (ordenes exactas de `ninja -t commands`), ar, enlace, nro.
# Salida: r9/r38c/bin/<nombre>_pgo.{nro,elf} (en esta laptop; traer con remoto.sh pull).
set -euo pipefail
export LC_ALL=C  # diff en ingles: el sed de abajo lee "Files ... differ"
N=${1:?nombre}
P=$HOME/nsmbw-port; C=$P/r9/r17pgo; L=$P/r9/r38c/nWiiRuntime
S=$C/c38src_$N; B=$C/nx/c38_$N
cd $C
rm -rf "$S" "$B"
mkdir -p "$S"
cp -a --reflink=auto m5src/nWiiRuntime "$S/nWiiRuntime"
# las fuentes de r38c (solo .cpp/.h/.inc de src/; include/ debe ser igual)
diff -rq m5src/nWiiRuntime/include "$L/include" && echo "include/ igual"
FILES=$(cd "$L" && { diff -rq "$S/nWiiRuntime/src" src || true; } | sed -n 's#^Files .* and src/\(.*\) differ$#src/\1#p')
echo "cambian: $FILES"
[[ -n $FILES ]] || { echo "no cambia nada?" >&2; exit 4; }
for f in $FILES; do cp "$L/$f" "$S/nWiiRuntime/$f"; done
cp -a --reflink=auto nx/m5_pgo "$B"
ninja -C nx/m5_pgo -t commands nsmbw.nro > "$B/m5_cmds.txt"
sed -e "s#nx/m5_pgo#nx/c38_$N#g" "$B/m5_cmds.txt" > "$B/cmds.txt"
# fuentes a recompilar: las cambiadas + las que incluyen las cabeceras cambiadas
RE=$(cd "$S/nWiiRuntime" && { for f in $FILES; do echo "$f"; done; grep -rl 's2\.h\|switch_platform\.h' src; } | sort -u | sed 's#^src/##' | paste -sd'|')
echo "recompilar: $RE"
n=0
while IFS= read -r line; do
  src=$(grep -oE -- '-c [^ ]+' <<<"$line" | cut -c4- || true)
  rel=$(sed -nE 's#.*/(m4src|m5src)/nWiiRuntime/src/##p' <<<"$src")
  [[ -n $rel ]] && grep -qxE "$RE" <<<"$rel" || continue
  new=$(sed -E "s#$C/(m4src|m5src)/nWiiRuntime/src/$rel#$S/nWiiRuntime/src/$rel#" <<<"$line")
  echo "== $rel"
  (cd "$B" && nice -n 5 bash -c "$new")
  n=$((n + 1))
done < "$B/cmds.txt"
echo "compilados: $n"
grep -E 'gcc-ar qc runtime/nwiiruntime/libnwiiruntime.a|-o nsmbw.elf|nacptool|elf2nro' "$B/cmds.txt" > "$B/final.txt"
[[ $(wc -l < "$B/final.txt") == 4 ]] || { echo "faltan ordenes finales" >&2; exit 3; }
while IFS= read -r line; do (cd "$B" && nice -n 5 bash -c "$line"); done < "$B/final.txt"
python3 tools/check_fm_tab_g5.py "$B/nsmbw.elf" 2>&1 | tail -2
mkdir -p $P/r9/r38c/bin
cp "$B/nsmbw.nro" "$P/r9/r38c/bin/${N}_pgo.nro"; cp "$B/nsmbw.elf" "$P/r9/r38c/bin/${N}_pgo.elf"
sha256sum "$P/r9/r38c/bin/${N}_pgo.nro" "$P/r9/r38c/bin/${N}_pgo.elf"
