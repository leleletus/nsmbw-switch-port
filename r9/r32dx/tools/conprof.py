#!/usr/bin/env python3
"""conprof.py <elf> <profile_cpu.txt> [N]: reparto de las muestras del hilo del juego de la consola (corrida entera).

ocioso (svcSleepThread / idle_wait / espera) | código generado (func_*, *_8XXXXXXX) | run_game | runtime (por símbolo).
Guarda datos/conprof_<nombre>.json: muestras por función (para el IPC por función de tools/dx_ipc.py).
"""
import bisect
import collections
import json
import os
import re
import subprocess
import sys

NM = "/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm"
GEN = re.compile(r"(^func_|_8[0-9A-Fa-f]{7}$)")
elf, prof = sys.argv[1], sys.argv[2]
topn = int(sys.argv[3]) if len(sys.argv) > 3 else 30
syms = []
for ln in subprocess.run([NM, "-n", "-S", "-C", "--defined-only", elf], capture_output=True, text=True).stdout.splitlines():
    q = ln.split(None, 3)
    if len(q) == 4 and q[2] in "tTwW" and re.fullmatch(r"[0-9a-f]+", q[1]):
        syms.append((int(q[0], 16), int(q[1], 16), q[3]))
    else:
        q = ln.split(None, 2)
        if len(q) == 3 and q[1] in "tTwW":
            syms.append((int(q[0], 16), 0, q[2]))
st = [s[0] for s in syms]
base, tot = None, 0
by = collections.Counter()
hdr = {}
for ln in open(prof):
    p = ln.split()
    if not p or p[0].startswith("#"):
        continue
    if p[0] == "base":
        base = int(p[1], 16)
    elif len(p) == 3 and p[0].isdigit():
        n, pc = int(p[0]), int(p[1], 16) - base
        i = bisect.bisect_right(st, pc) - 1
        if i >= 0 and (pc < syms[i][0] + syms[i][1] or (syms[i][1] == 0 and (i + 1 == len(syms) or pc < syms[i + 1][0]))):
            nm = syms[i][2]
        else:
            nm = "?"
        by[nm] += n
        tot += n
    elif len(p) >= 2:
        hdr[p[0]] = p[1]
idle = sum(v for k, v in by.items() if "svcSleepThread" in k or "idle_wait" in k or "svcWaitSynchronization" in k or "svcWaitForAddress" in k)
gen = sum(v for k, v in by.items() if GEN.search(k.split("(")[0]))
rg = by.get("run_game", 0)
busy = tot - idle
print(f"muestras {tot} (intervalo {hdr.get('interval_us')} us, {hdr.get('elapsed_s')} s): ocioso {idle} ({100 * idle / tot:.1f} %), ocupado {busy}")
print(f"  del ocupado: código generado {100 * gen / busy:.1f} %, run_game {100 * rg / busy:.1f} %, runtime y bibliotecas {100 * (busy - gen - rg) / busy:.1f} %")
print("  runtime (no generado) más caro, % del ocupado:")
k = 0
for nm, v in by.most_common():
    if GEN.search(nm.split("(")[0]) or nm == "run_game" or "svcSleepThread" in nm or "idle_wait" in nm:
        continue
    print(f"   {100 * v / busy:5.2f}  {nm[:110]}")
    k += 1
    if k >= topn:
        break
json.dump({"tot": tot, "idle": idle, "busy": busy, "gen": gen, "run_game": rg, "by": dict(by)},
          open(os.path.join("/home/djvemo/nsmbw-port/r9/r32dx/datos", "conprof_" + os.path.basename(os.path.dirname(os.path.abspath(prof))) + ".json"), "w"))
