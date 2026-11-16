#!/usr/bin/env python3
"""symhist.py <elf> <profile_cpu.txt> [N]: muestras del hilo del juego por simbolo (sin la espera)."""
import sys, subprocess, bisect, collections, re
elf, prof = sys.argv[1], sys.argv[2]
N = int(sys.argv[3]) if len(sys.argv) > 3 else 60
NM = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm"
syms = []
for l in subprocess.run([NM, "-n", "-C", elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split(None, 2)
    if len(p) == 3 and p[1] in "tTwW":
        syms.append((int(p[0], 16), p[2]))
addrs = [s[0] for s in syms]
base = None; tot = 0; h = collections.Counter()
GEN = re.compile(r"(^func_|^run_game$|_8[0-9A-Fa-f]{7}(_g6)?$)")
gen = 0
for l in open(prof):
    p = l.split()
    if l.startswith("base"): base = int(p[1], 16); continue
    if len(p) != 3 or not p[1].startswith("0x"): continue
    c = int(p[0]); pc = int(p[1], 16) - base
    i = bisect.bisect_right(addrs, pc) - 1
    n = syms[i][1] if i >= 0 else "?"
    tot += c
    if GEN.search(n): gen += c
    else: h[n] += c
print(f"total {tot}  generado {gen} ({100*gen/tot:.1f}%)")
for n, c in h.most_common(N):
    print(f"{c:8d} {100*c/tot:6.2f}%  {n[:110]}")
