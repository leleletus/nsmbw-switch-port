#!/usr/bin/env bash
# [r45p] nro_input.sh <dir_enlace> <src_runtime> <salida>: NRO de PRUEBA con entrada guionizada (NWII_INPUT_SCRIPT /
# NWII_INPUT_PHASES / [SCENE]) = el MISMO enlace final de un NRO ya hecho con orden L (k_cmds.txt + order/order.txt
# de order_m6.sh, p. ej. r9/r41m/nx/m6i) + input_script_nx.o + --wrap de KPADRead/KPADReadEx (como
# r9/r17pgo/tools/test_nro.sh). Solo para medir con el banco (nxlink); NUNCA instalarlo como juego.
#   <dir_enlace>: carpeta en la Omen con k_cmds.txt y order/order.txt (no se modifica: solo se lee)
#   <src_runtime>: carpeta del runtime en la Omen (include/ y src/) con que se compiló ese NRO (p. ej. r41m/src_m6irt)
#   <salida>: nombre; resultado r9/r45p/bin/<salida>_input_test.nro/.elf (+ .sha256)
set -euo pipefail
B=${1:?dir_enlace}; T=${2:?src_runtime}; OUTN=${3:?salida}
P=/home/djvemo/nsmbw-port/r9/r45p
mkdir -p $P/bin
ssh nsmbw-build "mkdir -p $P/nx/$OUTN $P/tools"
rsync -a /home/djvemo/nsmbw-port/r9/r17pgo/tools/input_script.cpp nsmbw-build:$P/tools/
ssh nsmbw-build /home/djvemo/nsmbw-port/scripts/lim bash -s -- "$B" "$T" "$OUTN" <<'EOF'
set -euo pipefail
export LC_ALL=C DKP=/opt/devkitpro DEVKITPRO=/opt/devkitpro
B=$1; T=$2; OUTN=$3; P=/home/djvemo/nsmbw-port/r9/r45p; D=$P/nx/$OUTN
INPUTO=$D/input_script_nx.o
$DKP/devkitA64/bin/aarch64-none-elf-g++ -DNWII_PLATFORM_SWITCH=1 -DNWII_FASTMEM=1 \
  -I"$T/include" -I"$T/src" -I"$B/_deps/glad-build/include" \
  -isystem $DKP/portlibs/switch/include -isystem $DKP/portlibs/switch/include/SDL2 -isystem $DKP/libnx/include \
  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -ftls-model=local-exec \
  -ffunction-sections -fdata-sections -D__SWITCH__ -O2 -DNDEBUG -ffp-contract=off -fomit-frame-pointer -fvisibility=hidden \
  -std=gnu++20 -fPIC -D_REENTRANT -c "$P/tools/input_script.cpp" -o "$INPUTO"
lk=$(grep -E -- '-o nsmbw\.elf' "$B/k_cmds.txt"); lk=${lk#: && }; lk=${lk% && :}
[[ $lk == *' -fPIE '* ]] || { echo "nro_input: el enlace no lleva -fPIE"; exit 2; }
lk=${lk/ -o nsmbw.elf / -Wl,--section-ordering-file=$B/order/order.txt -o $D/nsmbw.elf }
lk=${lk/ -fPIE / -fPIE $INPUTO -Wl,--wrap=NWII_KPADRead,--wrap=NWII_KPADReadEx }
cd "$B"; rm -f "$D/nsmbw.elf" "$D/nsmbw.nro"
nice -n 5 bash -c "$lk" > "$D/link.log" 2>&1 || { tail -20 "$D/link.log"; exit 3; }
$DKP/tools/bin/nacptool --create "NSMBW Recomp" nsmbw-port 0.2 "$D/nsmbw.nacp"
$DKP/tools/bin/elf2nro "$D/nsmbw.elf" "$D/nsmbw.nro" --icon=$DKP/libnx/default_icon.jpg --nacp="$D/nsmbw.nacp"
python3 /home/djvemo/nsmbw-port/r9/r17pgo/tools/check_fm_tab_g5.py "$D/nsmbw.elf" 2>&1 | tail -2
EOF
rsync -a "nsmbw-build:$P/nx/$OUTN/nsmbw.nro" "$P/bin/${OUTN}_input_test.nro"
rsync -a "nsmbw-build:$P/nx/$OUTN/nsmbw.elf" "$P/bin/${OUTN}_input_test.elf"
(cd $P/bin && sha256sum ${OUTN}_input_test.nro ${OUTN}_input_test.elf | tee ${OUTN}_input_test.sha256)
