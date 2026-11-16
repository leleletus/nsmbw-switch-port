#!/usr/bin/env bash
# build_hot.sh <name> <profiles...>   (correr con ~/nsmbw-port/scripts/lim; el nombre build_*.sh toma turno de compilación)
# r19n: copia de r9/r18m/tools/build_hot.sh (M=r19n; además mete las variantes nativas n1v_* junto a su
# función generada y native_n1.o del runtime en la zona caliente). Frente L (r9/r16l) aplicado a r18m: re-enlaza la NRO de Switch <name> (enlace guardado por build_m.sh en
# nx/link_<name>.cmd) 1) sin orden y con mapa, 2) con el orden "lo caliente primero" de r9/tools/hot_order.py
# hecho con los perfiles dados (PERFIL@ELF[:PESO]), comprueba (hot_order_eval/eqcheck) y saca
#   bin/nsmbw_<name>_hot.nro/.elf  (+ _input_test.nro con el mismo orden; solo pruebas)
set -euo pipefail
M=/home/djvemo/nsmbw-port/r9/r19n; T=/home/djvemo/nsmbw-port/r9/tools
BASE=${BASE:-/home/djvemo/nsmbw-port/r9/r14g5}; AB=$BASE/nx/build
DKP=/opt/devkitpro; ICON=$DKP/libnx/default_icon.jpg
n=$1; shift
cmd=$(cat "$M/nx/link_$n.cmd")
D=$M/nx/hot/$n; mkdir -p "$D/base" "$D/hot" "$D/hot_test" "$M/nx/hot/order"
WORKER=(--static-root '_ZZL11StartWorkervENUlPvE_4_FUNES_' --static-root '_ZZL10WorkerMainv')
# 1) sin orden, con mapa
(cd "$AB" && eval "${cmd/-o nsmbw.elf/-o $D/base/nsmbw.elf -Wl,-Map=$D/base/nsmbw.map}")
cmp <(/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objcopy -O binary -j .text "$D/base/nsmbw.elf" /dev/stdout | sha256sum) \
    <(/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objcopy -O binary -j .text "$M/bin/nsmbw_$n.elf" /dev/stdout | sha256sum) \
  && echo "[hot] enlace base: .text idéntico a bin/nsmbw_$n.elf"
# 2) orden
O=$M/nx/hot/order/$n
python3 $T/hot_order.py --map "$D/base/nsmbw.map" --elf "$D/base/nsmbw.elf" --out "$O.txt" --report "${O}_informe.txt" \
  --by-thread "${WORKER[@]}" "$@" | tee "${O}_log.txt"
# r19n: cada n1v_<dir> (variante nativa, sección propia) va justo después de su función generada; las
# funciones del runtime de native_n1.o, delante de todo lo del runtime
python3 - "$O.txt" "$D/base/nsmbw.map" <<'PY'
import re, sys
o, mp = sys.argv[1], sys.argv[2]
secs = {}
for m in re.finditer(r'(\.text\._ZL12n1v_([0-9A-F]{8})\S*)\s', open(mp).read()):
    secs[m.group(2)] = m.group(1)
out, added = [], 0
for ln in open(o).read().split('\n'):
    out.append(ln)
    m = re.match(r'^(\s*\*/mod_output_\d+\.o)\(\.text\.\w+_([0-9A-F]{8})\)$', ln)
    if m and m.group(2) in secs:
        out.append('%s(%s)' % (m.group(1), secs.pop(m.group(2))))
        added += 1
    if ln.strip() == 'KEEP(*(.crt0))':
        out.append('  *libnwiiruntime.a:native_n1.o(.text.*)')
open(o, 'w').write('\n'.join(out))
print('[hot] r19n: %d variantes n1v_ colocadas junto a su función; sin perfil (al final): %s' % (added, sorted(secs)))
PY
(cd "$AB" && eval "${cmd/-o nsmbw.elf/-o $D/hot/nsmbw.elf -Wl,--section-ordering-file=$O.txt -Wl,-Map=$D/hot/nsmbw.map}")
# 3) comprobaciones (mismas secciones, datos en su sitio; código igual salvo reubicaciones)
P1=${EVAL_PROFILE:-}
python3 $T/hot_order_eval.py --base-map "$D/base/nsmbw.map" --base-elf "$D/base/nsmbw.elf" \
  --new-map "$D/hot/nsmbw.map" --new-elf "$D/hot/nsmbw.elf" $P1 | tee "${O}_eval.txt" || \
  echo "[hot] r19n: hot_order_eval no da todo igual (si solo es .text unos bytes más larga por alineación y las demás secciones desplazadas, vale; lo decide eqcheck: solo __nx_mod0 puede salir distinta)"
python3 $T/hot_order_eqcheck.py "$D/base/nsmbw.elf" "$D/hot/nsmbw.elf" | tee "${O}_eqcheck.txt" || \
  { [[ $(grep -c '^   distinta:' "${O}_eqcheck.txt") == 1 ]] && grep -q '^   distinta: __nx_mod0 ' "${O}_eqcheck.txt" && \
    echo "[hot] r19n: la única distinta es __nx_mod0 (desplazamiento a .dynamic, cambia con el tamaño de .text): vale"; }
# 4) NRO
$DKP/tools/bin/nacptool --create "NSMBW PRUEBA n5s (nativas SDK + demos fijas)" "nsmbw-port" "n5s" "$D/hot/nsmbw.nacp" >/dev/null
$DKP/tools/bin/elf2nro "$D/hot/nsmbw.elf" "$M/bin/nsmbw_${n}_hot.nro" --icon=$ICON --nacp="$D/hot/nsmbw.nacp" >/dev/null
cp -f "$D/hot/nsmbw.elf" "$M/bin/nsmbw_${n}_hot.elf"
# 5) _input_test con el mismo orden (solo pruebas guionizadas)
INPUTO=$M/nx/build/input_script_nx.o
tcmd=${cmd/-o nsmbw.elf/-o $D/hot_test/nsmbw.elf -Wl,--section-ordering-file=$O.txt}
tcmd=${tcmd/ -fPIE / -fPIE $INPUTO -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }
(cd "$AB" && eval "$tcmd")
$DKP/tools/bin/elf2nro "$D/hot_test/nsmbw.elf" "$M/bin/nsmbw_${n}_hot_input_test.nro" --icon=$ICON --nacp="$D/hot/nsmbw.nacp" >/dev/null
(cd "$M/bin" && sha256sum "nsmbw_${n}_hot.nro" "nsmbw_${n}_hot.elf" "nsmbw_${n}_hot_input_test.nro")
