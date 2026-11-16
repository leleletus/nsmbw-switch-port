#!/usr/bin/env bash
# [r41m] order_m6.sh (copia de r42k/tools/order_k.sh: salidas en r41m; objetos KOBJ de r42k/nx/<KOBJ>, solo lectura)
# Original: order_k.sh <tag> <nuevo_tag> <perfil>@<elf>[:peso] ... : orden de enlace L («lo caliente primero»,
# r9/tools/hot_order.py, frente L / r16l) encima de una compilación ya hecha r42k/nx/<tag> (o «m6a» = r41m/nx/m6a).
# Sin recompilar: mismos objetos; re-enlaza con -Wl,--section-ordering-file. Copia de r17pgo/tools/order_relink.sh
# adaptada (el comando de enlace sale de k_cmds.txt / cmds.txt y no de ninja). Perfiles = profile_{cpu,main,gxw}.txt
# de corridas de consola + el .elf con que se tomaron (se traducen por nombre de símbolo).
# KOBJ=<tag>: objetos del código generado de r42k/nx/<tag> sobre la base <tag> (p. ej. «r41m:m6e»).
# Resultado: r9/r42k/bin/<nuevo_tag>.nro/.elf (+ .sha256); orden e informe en r42k/nx/<nuevo_tag>/order/.
set -euo pipefail
TAG=${1:?tag}; NT=${2:?nuevo tag}; shift 2
L=/home/djvemo/nsmbw-port/r9/r41m
mkdir -p $L/logs
ssh nsmbw-build "mkdir -p $L/order_in/$NT $L/tools"; rsync -a $L/tools/cruces.py $L/tools/orden_pares.py nsmbw-build:$L/tools/
P=()
for a in "$@"; do
  prof=${a%%@*}; rest=${a#*@}; elf=${rest%%:*}; w=""; [[ $rest == *:* ]] && w=":${rest#*:}"
  run=$(basename "$(dirname "$prof")")
  dp="$L/order_in/$NT/${run}_$(basename "$prof")"; de="$L/order_in/$NT/$(basename "$elf")"
  rsync -a "$prof" "nsmbw-build:$dp"; rsync -a "$elf" "nsmbw-build:$de"
  P+=("$dp@$de$w")
done
{ printf 'KOBJ=%q\n' "${KOBJ:-}"; printf 'KOBJDIR=%q\n' "${KOBJDIR:-}"; printf 'set -- %q %q' "$TAG" "$NT"; ((${#P[@]})) && printf ' %q' "${P[@]}"; echo; sed -n '/^#==REMOTO==$/,$p' "$0"; } |
  ssh nsmbw-build /home/djvemo/nsmbw-port/scripts/lim bash -s 2>&1 | tee $L/logs/order_$NT.log
[[ ${PIPESTATUS[1]} == 0 ]] || { echo "[k] fallo remoto"; exit 3; }
rsync -a "nsmbw-build:$L/nx/$NT/nsmbw.nro" "$L/bin/$NT.nro"
rsync -a "nsmbw-build:$L/nx/$NT/nsmbw.elf" "$L/bin/$NT.elf"
mkdir -p $L/order; rsync -a "nsmbw-build:$L/nx/$NT/order/" "$L/order/$NT/" --exclude '*.elf' --exclude '*.map'
(cd $L/bin && sha256sum $NT.nro $NT.elf | tee $NT.sha256)
exit 0
#==REMOTO==
set -euo pipefail
export LC_ALL=C DEVKITPRO=/opt/devkitpro
TAG=$1 NT=$2; shift 2
L=/home/djvemo/nsmbw-port/r9/r41m; K=/home/djvemo/nsmbw-port/r9/r42k; T=/home/djvemo/nsmbw-port/r9/tools; DKP=/opt/devkitpro
# <tag>: una compilación de aquí (nx/<tag>, k_cmds.txt) o una del frente M: «m6a» o «r41m:<n>» (r41m/nx/<n>, cmds.txt)
case $TAG in
  m6a) S=/home/djvemo/nsmbw-port/r9/r41m/nx/m6a; C=$S/cmds.txt ;;
  r41m:*) S=/home/djvemo/nsmbw-port/r9/r41m/nx/${TAG#r41m:}; C=$S/cmds.txt ;;
  *) S=$L/nx/$TAG; C=$S/k_cmds.txt ;;
esac
B=$L/nx/$NT
[[ $B != "$S" ]] || { echo "nuevo tag = tag"; exit 2; }
rm -rf "$B"; cp -a --reflink=auto "$S" "$B"
sed -e "s#$S#$B#g" "$C" > "$B/k_cmds.txt"
# KOBJ=<tag de aquí>: los objetos del código generado (los 88 con perfil) se toman de nx/<KOBJ> (p. ej. k2 sobre el
# runtime de m6e); main_output.o y el runtime son los de <tag>
if [[ -n $KOBJ ]]; then
  OD=CMakeFiles/nsmbw.dir/home/djvemo/nsmbw-port/r9/r17pgo/m4src/export
  n=0; KD=${KOBJDIR:-$K/nx/$KOBJ}; echo "[k] objetos de $KD"
  for o in $KD/$OD/output_*.o $KD/$OD/fastdispatch.o; do cp "$o" "$B/$OD/"; n=$((n + 1)); done
  echo "[k] objetos del código generado de $KOBJ: $n"; [[ $n == 88 ]] || { echo "[k] esperaba 88"; exit 2; }
fi
cd "$B"; mkdir -p order/base order/ord
lk=$(grep -E -- '-o nsmbw\.elf' k_cmds.txt)
lk=${lk#: && }; lk=${lk% && :}
# enlace base (sin orden) con mapa (--noinhibit-exec: con partición puede fallar por saltos condicionales lejanos)
nice -n 5 bash -c "${lk/ -o nsmbw.elf / -Wl,--noinhibit-exec -Wl,-Map=order\/base\/nsmbw.map -o order\/base\/nsmbw.elf }" > order/link_base.log 2>&1 || true
if [[ -s nsmbw.elf ]]; then
cmp <($DKP/devkitA64/bin/aarch64-none-elf-objcopy -O binary -j .text order/base/nsmbw.elf /dev/stdout) \
    <($DKP/devkitA64/bin/aarch64-none-elf-objcopy -O binary -j .text nsmbw.elf /dev/stdout) > /dev/null \
  && echo "[k] enlace base: .text igual que $TAG" || echo "[k] AVISO enlace base: .text distinto de $TAG"
fi
LORD=""
if (( $# > 0 )); then
python3 $T/hot_order.py --map order/base/nsmbw.map --elf order/base/nsmbw.elf --out order/order_L.txt \
  --report order/informe.txt --by-thread \
  --static-root '_ZZL11StartWorkervENUlPvE_4_FUNES_' --static-root '_ZZL10WorkerMainv' "$@" | tee order/order_log.txt
LORD=order/order_L.txt
fi
# saltos condicionales entre secciones (partición caliente/fría): juntar origen y destino
python3 $L/tools/cruces.py CMakeFiles/nsmbw.dir/home/djvemo/nsmbw-port/r9/r17pgo/m4src/export > order/cruces.tsv
python3 $L/tools/orden_pares.py order/cruces.tsv order/order.txt $LORD
rm -f nsmbw.elf nsmbw.nro
nice -n 5 bash -c "${lk/ -o nsmbw.elf / -Wl,--section-ordering-file=order\/order.txt -Wl,-Map=order\/ord.map -o nsmbw.elf }" > order/link_ord.log 2>&1 || { tail -20 order/link_ord.log; exit 3; }
python3 $T/hot_order_eqcheck.py order/base/nsmbw.elf nsmbw.elf > order/eqcheck.txt 2>&1 || true
grep '^\[eq\]' order/eqcheck.txt || true
grep '^   distinta' order/eqcheck.txt | awk '{print $2}' | sort -u | while read -r s; do
  k=$($DKP/devkitA64/bin/aarch64-none-elf-nm order/base/nsmbw.elf | grep -c " $s\$" || true)
  ((k > 1)) || echo "[k] DIFERENCIA REAL en $s (nombre único): mirar"
done
nice -n 5 bash -c "$(grep nacptool k_cmds.txt)" >> order/link_ord.log 2>&1
nice -n 5 bash -c "$(grep elf2nro k_cmds.txt)" >> order/link_ord.log 2>&1
python3 /home/djvemo/nsmbw-port/r9/r17pgo/tools/check_fm_tab_g5.py nsmbw.elf 2>&1 | tail -2
rm -f order/base/nsmbw.elf
