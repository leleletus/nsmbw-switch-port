#!/usr/bin/env bash
# tools/order_relink.sh <variant> <profile>[@elf][:weight] ...: front L's "hot first" link order
# (r9/tools/hot_order.py) on top of a finished build_pgo.sh build nx/<variant> -> bin/<variant>_ord.nro
# No recompilation: same objects, only the order of the sections in .text changes.
# Profiles: logs/profile_{cpu,main}.txt of console runs; @elf = the .elf the NRO of that run was
# built with (translated by symbol name when it is another build; best: a profile of <variant> itself).
# Run through ~/nsmbw-port/scripts/lim (two 100+ MB links).
set -euo pipefail
C=$(cd "$(dirname "$0")/.." && pwd)
T=$C/../tools
V=${1:?usage: order_relink.sh <variant> <profile>...}; shift
B=$C/nx/$V
O=$B/order
DKP=${DEVKITPRO:-/opt/devkitpro}
mkdir -p "$O/base" "$O/ord"
memwait() {
  while (( $(awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo) < ${MINFREE_MB:-7000} )) ||
        pgrep -x 'ld|ld.bfd|collect2' > /dev/null; do sleep 10; done
}
python3 "$T/hot_order_linkargs.py" "$B" "$O/link_args.txt"
mapfile -t args < "$O/link_args.txt"
memwait
(cd "$B" && nice -n 5 "${args[@]}" -Wl,-Map="$O/base/nsmbw.map" -o "$O/base/nsmbw.elf")
python3 "$T/hot_order.py" --map "$O/base/nsmbw.map" --elf "$O/base/nsmbw.elf" --out "$O/order.txt" \
  --report "$O/informe.txt" --by-thread \
  --static-root '_ZZL11StartWorkervENUlPvE_4_FUNES_' --static-root '_ZZL10WorkerMainv' "$@" | tee "$O/order_log.txt"
memwait
# output named nsmbw.elf: --nx-module-name takes the file name
(cd "$B" && nice -n 5 "${args[@]}" -Wl,--section-ordering-file="$O/order.txt" -Wl,-Map="$O/ord/nsmbw.map" -o "$O/ord/nsmbw.elf")
# Code check (same code, only relocations differ). With PGO, "distinta" can show up for LOCAL clones that
# exist under the same name in several objects (psq_store...isra.0, fcmp...constprop.0: each TU has its own,
# optimized with its own profile) because the tool pairs symbols by name: check those names are duplicated.
python3 "$T/hot_order_eqcheck.py" "$O/base/nsmbw.elf" "$O/ord/nsmbw.elf" > "$O/eqcheck.txt" 2>&1 || true
grep '^\[eq\]' "$O/eqcheck.txt"
grep '^   distinta' "$O/eqcheck.txt" | awk '{print $2}' | sort -u | while read -r s; do
  k=$($DKP/devkitA64/bin/aarch64-none-elf-nm "$O/base/nsmbw.elf" | grep -c " $s\$" || true)
  ((k > 1)) || echo "REAL DIFFERENCE in $s (unique name): check it"
done
$DKP/tools/bin/nacptool --create "NSMBW Recomp" nsmbw-port 0.2 "$O/ord/nsmbw.nacp" > /dev/null
$DKP/tools/bin/elf2nro "$O/ord/nsmbw.elf" "$C/bin/${V}_ord.nro" --icon=$DKP/libnx/default_icon.jpg --nacp="$O/ord/nsmbw.nacp"
cp "$O/ord/nsmbw.elf" "$C/bin/${V}_ord.elf"
python3 "${FMCHECK:-$C/tools/check_fm_tab.py}" "$C/bin/${V}_ord.elf" 2>&1 | tail -2
# the same order for the citron/scripted-input test NRO (training/measuring only, never as the game)
TEST_LINK_EXTRA="-Wl,--section-ordering-file=$O/order.txt" OUT_SUFFIX=_ord "$C/tools/test_nro.sh" "$V" > /dev/null
ls -la "$C/bin/${V}_ord.nro" "$C/bin/${V}_ord_input_test.nro"
