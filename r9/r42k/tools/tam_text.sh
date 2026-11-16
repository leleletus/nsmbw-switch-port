#!/usr/bin/env bash
# [r42k] tam_text.sh <tag>... : tamaño del código generado (los 88 .o con perfil + main_output) por tipo de sección,
# sumado sobre los objetos de r9/r42k/nx/<tag> (o r41m/nx/m6a con tag "m6a"), en KB:
#   hot = .text.hot.*   normal = .text.<fn>   unlikely = .text.unlikely.* (funciones enteras frías + partes .cold)
#   cold = de eso, lo que son partes .cold (bloques separados de funciones que sí se ejecutan)
#   rodata = .rodata* (aquí van las tablas de saltos de los switch)
for TAG in "$@"; do
  { echo "TAG=$TAG"; sed -n '/^#==R==$/,$p' "$0"; } | ssh nsmbw-build bash -s
done
exit 0
#==R==
cd /home/djvemo/nsmbw-port/r9
D=r42k/nx/$TAG; [[ $TAG == m6a ]] && D=r41m/nx/m6a
O=$D/CMakeFiles/nsmbw.dir/home/djvemo/nsmbw-port/r9/r17pgo/m4src/export
T=/opt/devkitpro/devkitA64/bin/aarch64-none-elf
for f in $O/output_*.o $O/fastdispatch.o; do $T-size -A $f; done | awk -v tag=$TAG '
  $1 ~ /^\.text\.hot/ {h+=$2; next}
  $1 ~ /^\.text\.unlikely/ {u+=$2; next}
  $1 ~ /^\.text/ {n+=$2; next}
  $1 ~ /^\.rodata/ {r+=$2}
  END {printf "%-10s hot %8.0f  normal %8.0f  unlikely %8.0f  total %8.0f KB | rodata %6.0f KB", tag, h/1024, n/1024, u/1024, (h+n+u)/1024, r/1024}'
for f in $O/output_*.o $O/fastdispatch.o; do $T-nm -S $f | grep '\.cold$'; done | awk '{s+=strtonum("0x"$2); k++} END{printf " | .cold %d partes %.0f KB\n", k, s/1024}'
