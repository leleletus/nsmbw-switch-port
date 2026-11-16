#!/usr/bin/env bash
# [r42k] build_k.sh <tag> : NRO de Switch = m6a (r41m/nx/m6a: runtime m6a sin perfil + objetos PGO del juego de m5)
# con el CÓDIGO GENERADO (output_*.cpp + fastdispatch.cpp, los 88 fuentes que llevan -fprofile-use) recompilado con
# flags extra. MISMO perfil (r17pgo/profiles/m4_all), mismas órdenes de ninja; main_output.cpp y el runtime no se tocan.
# Corre en ESTA laptop; compila en la otra (ssh nsmbw-build) en r9/r42k/nx/<tag> (copia --reflink de r41m/nx/m6a).
# Variables:
#   KFLAGS="..."      flags extra para todos los fuentes generados con perfil (se ponen antes de -MD)
#   KRE=regex KRE_FLAGS="..."  flags extra (además de KFLAGS) solo para los fuentes cuyo nombre case con regex
#   KONLY=regex       recompilar solo esos fuentes (prueba rápida); el resto se queda como en m6a
#   KLINK="..."       flags extra de enlace (p. ej. -Wl,--section-ordering-file=...)
#   KFILES="a b"      archivos locales a copiar a nx/<tag>/ antes de enlazar (p. ej. el fichero de orden)
#   JOBS=6            compilaciones a la vez en la otra laptop
#   NOLINK=1          no enlazar (solo objetos; para pruebas de un fuente)
# Resultado: r9/r42k/bin/<tag>.nro/.elf (+ .sha256), registro r9/r42k/logs/build_<tag>.log
set -euo pipefail
TAG=${1:?tag}
L=/home/djvemo/nsmbw-port/r9/r42k
mkdir -p $L/bin $L/logs
ssh nsmbw-build "mkdir -p $L/nx"
for f in ${KFILES:-}; do rsync -a "$f" "nsmbw-build:$L/nx/kfiles_${TAG}_$(basename $f)"; done
{ printf 'set -- %q %q %q %q %q %q %q %q %q\n' "$TAG" "${KFLAGS:-}" "${KRE:-}" "${KRE_FLAGS:-}" "${KONLY:-}" \
    "${KLINK:-}" "${JOBS:-6}" "${NOLINK:-0}" "${KFILES:-}"; sed -n '/^#==REMOTO==$/,/^#==FIN==$/p' "$0"; } |
  ssh nsmbw-build /home/djvemo/nsmbw-port/scripts/lim bash -s 2>&1 | tee $L/logs/build_$TAG.log
[[ ${PIPESTATUS[1]} == 0 ]] || { echo "[k] fallo remoto"; exit 3; }
[[ ${NOLINK:-0} == 1 ]] && exit 0
rsync -a "nsmbw-build:$L/nx/$TAG/nsmbw.nro" "$L/bin/$TAG.nro"
rsync -a "nsmbw-build:$L/nx/$TAG/nsmbw.elf" "$L/bin/$TAG.elf"
(cd $L/bin && sha256sum $TAG.nro $TAG.elf | tee $TAG.sha256)
exit 0
#==REMOTO==
set -euo pipefail
export LC_ALL=C DEVKITPRO=/opt/devkitpro
TAG=$1 KFLAGS=$2 KRE=$3 KRE_FLAGS=$4 KONLY=$5 KLINK=$6 JOBS=$7 NOLINK=$8 KFILES=$9
L=/home/djvemo/nsmbw-port/r9/r42k; M=/home/djvemo/nsmbw-port/r9/r41m/nx/m6a; B=$L/nx/$TAG
rm -rf "$B"; cp -a --reflink=auto "$M" "$B"
for f in $KFILES; do mv "$L/nx/kfiles_${TAG}_$(basename $f)" "$B/$(basename $f)"; done
sed -e "s#$M#$B#g" "$M/cmds.txt" > "$B/k_cmds.txt"
: > "$B/k_compile.txt"; n=0
while IFS= read -r line; do
  [[ $line == *-fprofile-use=* && $line == *" -c "* ]] || continue
  src=$(grep -oE -- '-c [^ ]+' <<<"$line" | cut -c4-); base=$(basename "$src")
  [[ -n $KONLY && ! $base =~ $KONLY ]] && continue
  fl="$KFLAGS"; [[ -n $KRE && $base =~ $KRE ]] && fl="$fl $KRE_FLAGS"
  obj=$(grep -oE -- '-o [^ ]+' <<<"$line" | cut -c4-); rm -f "$B/$obj"
  echo "${line/ -MD / $fl -MD }" >> "$B/k_compile.txt"; n=$((n + 1))
done < "$B/k_cmds.txt"
echo "[k] $TAG: $n fuentes generados a recompilar; KFLAGS=[$KFLAGS] KRE=[$KRE] KRE_FLAGS=[$KRE_FLAGS] KLINK=[$KLINK]"
! grep -q "r41m/nx/m6a" "$B/k_compile.txt" || { echo "[k] quedan rutas de m6a"; exit 2; }
cd "$B"; t0=$(date +%s)
# los más grandes primero (output_22 ~0,9 GB de RAM)
while IFS= read -r line; do
  src=$(grep -oE -- '-c [^ ]+' <<<"$line" | cut -c4-); echo "$(stat -c %s "$src") $line"
done < k_compile.txt | sort -rn | cut -d' ' -f2- > k_compile_sorted.txt
mkdir -p k_logs; i=0
while IFS= read -r line; do
  i=$((i + 1))
  while (( $(jobs -rp | wc -l) >= JOBS )); do wait -n || true; done
  ( nice -n 10 bash -c "$line" > "k_logs/$i.log" 2>&1 || echo "FALLO $i" >> k_logs/FALLOS ) &
done < k_compile_sorted.txt
wait
echo "[k] compilación: $(( $(date +%s) - t0 )) s"
cat k_logs/*.log | grep -E 'warning|error|mismatch' | sort | uniq -c | sort -rn | head -20 || true
[[ ! -e k_logs/FALLOS ]] || { echo "[k] FALLOS:"; cat k_logs/FALLOS; for x in $(awk '{print $2}' k_logs/FALLOS); do tail -5 k_logs/$x.log; done; exit 3; }
[[ $NOLINK == 1 ]] && exit 0
rm -f nsmbw.elf nsmbw.nro
lk=$(grep -E -- '-o nsmbw\.elf' k_cmds.txt); lk=${lk/ -o nsmbw.elf / $KLINK -o nsmbw.elf }
nice -n 5 bash -c "$lk" > k_link.log 2>&1 || { tail -30 k_link.log; exit 3; }
nice -n 5 bash -c "$(grep nacptool k_cmds.txt)" >> k_link.log 2>&1
nice -n 5 bash -c "$(grep elf2nro k_cmds.txt)" >> k_link.log 2>&1
[[ -s nsmbw.elf && -s nsmbw.nro ]] || { echo "[k] sin elf/nro"; exit 3; }
python3 /home/djvemo/nsmbw-port/r9/r17pgo/tools/check_fm_tab_g5.py nsmbw.elf 2>&1 | tail -2
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-size -A nsmbw.elf | awk '$1 ~ /^\.text|^\.rodata$/ {printf "%s %d\n", $1, $2}'
#==FIN==
