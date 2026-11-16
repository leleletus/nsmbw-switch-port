#!/usr/bin/env bash
# fn_ins.sh <obj.o>...: "<function> <instructions>" per function (full names), devkitA64 objdump.
for o in "$@"; do
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objdump -d --no-show-raw-insn "$o" | awk '
/^[0-9a-f]+ <.*>:$/ { if (fn!="") print fn, n; fn=$2; gsub(/[<>:]/,"",fn); n=0; next }
/^ +[0-9a-f]+:\t/ { n++ } END { if (fn!="") print fn, n }'
done
