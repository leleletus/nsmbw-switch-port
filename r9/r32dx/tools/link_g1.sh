#!/usr/bin/env bash
# link_g1.sh   (con ~/nsmbw-port/scripts/lim)  ->  nx/dxg1/nsmbw.elf
# Enlaza EXACTAMENTE m2 (r24m2/nx/link_m2.cmd + orden L g6e2) cambiando solo los 87 objetos del juego por los de
# r32dx/nx_g1 (mismo fuente partido en una sentencia por línea, misma orden de compilación + -g1) y comprueba que
# .text sale idéntico byte a byte al de r24m2/bin/nsmbw_m2.elf: así la tabla de líneas DWARF de este ELF vale
# para las direcciones de m2 (la versión instalada).
set -euo pipefail
P=/home/djvemo/nsmbw-port; V=$P/r9/r32dx; M=$P/r9/r24m2; G5=$P/r9/r14g5; G=$P/r9/r20g6
D=$V/nx/dxg1; mkdir -p $D
[[ $(ls $V/nx_g1/output_*.o | wc -l) == 87 ]] || { echo "faltan objetos nx_g1" >&2; exit 2; }
cmd=$(cat $M/nx/link_m2.cmd)
cmd=${cmd//$G\/e2\/nx_obj\/output_/$V/nx_g1/output_}
[[ $cmd != *e2/nx_obj* ]] || { echo "quedan objetos de e2" >&2; exit 3; }
(cd $G5/nx/build && eval "${cmd/-o nsmbw.elf/-o $D/nsmbw.elf -Wl,--section-ordering-file=$M/nx/order/g6e2.txt}")
OC=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objcopy
$OC -O binary -j .text $D/nsmbw.elf $D/text_g1.bin
$OC -O binary -j .text $M/bin/nsmbw_m2.elf $D/text_m2.bin
if cmp -s $D/text_g1.bin $D/text_m2.bin; then echo "[dxg1] .text IDENTICO al de m2 ($(stat -c %s $D/text_m2.bin) bytes)"; rm -f $D/text_g1.bin $D/text_m2.bin
else echo "[dxg1] ERROR: .text difiere del de m2" >&2; cmp $D/text_g1.bin $D/text_m2.bin | head -3 >&2; exit 5; fi
ls -la $D/nsmbw.elf
