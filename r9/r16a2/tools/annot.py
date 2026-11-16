#!/usr/bin/env python3
# annot.py <elf> <profile.txt> <symbol-substring> [top=40]: profile samples per instruction of one function
import re, sys, subprocess, collections
elf, prof, name = sys.argv[1:4]; top = int(sys.argv[4]) if len(sys.argv) > 4 else 40
NM = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm"; OD = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-objdump"
syms = []
for l in subprocess.run([NM, "-S", elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split()
    if len(p) == 4 and name in p[3] and p[2] in "tT": syms.append((int(p[0], 16), int(p[1], 16), p[3]))
if not syms: sys.exit("no symbol")
lo, sz, sym = syms[0]
base = None; h = collections.Counter(); tot = 0
for l in open(prof):
    if l.startswith("base "): base = int(l.split()[1], 16)
    p = l.split()
    if len(p) == 3 and p[1].startswith("0x"):
        tot += int(p[0]); pc = int(p[1], 16) - base
        if lo <= pc < lo + sz: h[pc] += int(p[0])
dis = {}
for l in subprocess.run([OD, "-d", "--no-show-raw-insn", f"--start-address={lo:#x}", f"--stop-address={lo+sz:#x}", elf], capture_output=True, text=True).stdout.splitlines():
    m = re.match(r"\s*([0-9a-f]+):\s*(.*)", l)
    if m: dis[int(m.group(1), 16)] = m.group(2)[:80]
s = sum(h.values())
print(f"{sym} size {sz} samples {s} ({100*s/tot:.2f} % of all)")
for pc, c in h.most_common(top): print(f"{c:6d} {100*c/max(s,1):5.1f}% {pc:x} {dis.get(pc,'?')}")
