#!/usr/bin/env bash
# dis.sh <obj> <symbol> : disassembly of one function of an object (only its own section)
OD=/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objdump
$OD -d --no-show-raw-insn -j .text.$2 "$1" | awk 'f; /^[0-9a-f]+ <'"$2"'>:/{f=1}'
