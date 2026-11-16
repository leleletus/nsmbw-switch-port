#!/usr/bin/env bash
# fnstats.sh <obj.o>...: per-function AArch64 static counts (devkitA64 objdump): instructions,
# loads/stores relative to the ctx register (the register x0 is copied to at entry, or x0),
# to sp (stack spills/frame), guest accesses ([x, w, uxtw]), calls.
for o in "$@"; do
/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objdump -d --no-show-raw-insn "$o" | awk -v O="$(basename $o)" '
function flush() { if (fn!="" && fn !~ /^_ZN/) printf "%-14s %-36s ins=%6d ctxld=%5d ctxst=%5d spld=%4d spst=%4d guest=%5d calls=%4d\n", O, substr(fn,1,36), n, cl, cs, sl, ss, gm, bl }
/^[0-9a-f]+ <.*>:$/ { flush(); fn=$2; gsub(/[<>:]/,"",fn); n=cl=cs=sl=ss=gm=bl=0; ctx="x0"; first=1; next }
/^ +[0-9a-f]+:\t/ { n++; op=$2; args=$3; for(i=4;i<=NF;i++) args=args" "$i;
  if (first<=6 && op=="mov" && args ~ /, x0$/) { split(args,a,","); ctx=a[1]; } first++
  isld = (op ~ /^ld/); isst = (op ~ /^st/);
  if (isld||isst) {
    if (args ~ /uxtw\]/) gm++;
    else if (args ~ ("\\[" ctx "[,\\]]")) { if (isld) cl++; else cs++ }
    else if (args ~ /\[sp/) { if (isld) sl++; else ss++ }
  }
  if (op=="bl"||op=="blr") bl++ }
END { flush() }'
done
