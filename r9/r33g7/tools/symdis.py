#!/usr/bin/env python3
"""symdis.py <elf> <profile_cpu.txt> <symbol-substring> : desensamblado con muestras por instruccion."""
import sys, subprocess, collections
elf, prof, pat = sys.argv[1:4]
NM = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm"; OD = NM.replace("-nm", "-objdump")
syms = []
for l in subprocess.run([NM, "-n", "-S", "-C", elf], capture_output=True, text=True).stdout.splitlines():
    p = l.split(None, 3)
    if len(p) == 4 and p[2] in "tTwW": syms.append((int(p[0], 16), int(p[1], 16), p[3]))
hit = [s for s in syms if pat in s[2]]
base = None; h = collections.Counter()
for l in open(prof):
    p = l.split()
    if l.startswith("base"): base = int(p[1], 16); continue
    if len(p) == 3 and p[1].startswith("0x"): h[int(p[1], 16) - base] += int(p[0])
for a, sz, n in hit[:int(sys.argv[4]) if len(sys.argv) > 4 else 1]:
    tot = sum(c for pc, c in h.items() if a <= pc < a + sz)
    print(f"== {n} @{a:x} size {sz} samples {tot}")
    out = subprocess.run([OD, "-d", "--no-show-raw-insn", f"--start-address={a}", f"--stop-address={a+sz}", elf], capture_output=True, text=True).stdout
    for l in out.splitlines():
        p = l.split(":", 1)
        try: pc = int(p[0].strip(), 16)
        except ValueError: continue
        print(f"{h.get(pc, 0):6d} {l[:120]}")
