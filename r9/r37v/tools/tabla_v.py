#!/usr/bin/env python3
"""tabla_v.py <bench_dir>...: por corrida, comprobaciones (ventanas, [DEMO] start/end, reloj, imagen, [V]) y por escena
ms juego/fot REAL, fps juego, fps dibujados, render ms/fot (por_escena_real.py de r31t)."""
import sys, re, subprocess, os, glob
ESC = ["desierto4j", "agua_pinguino", "cielo_paratroopas", "nubes_plataformas", "castillo"]
rows = {}
for b in sys.argv[1:]:
    b = b.rstrip("/"); n = os.path.basename(b)
    log = open(os.path.join(b, "nwii.log"), errors="replace").read()
    res = open(os.path.join(b, "resumen.txt"), errors="replace").read() if os.path.exists(os.path.join(b, "resumen.txt")) else ""
    win = re.findall(r"\[bench\] (\d+) windows", res)
    st = len(re.findall(r"\[DEMO\][^\n]*start", log)); en = len(re.findall(r"\[DEMO\][^\n]*(?:end|fin de demo)", log))
    mhz = sorted(set(re.findall(r"cpu (\d+) MHz, gpu (\d+) MHz", log)))
    img = re.findall(r"IMAGEN[^\n]*", res)
    held = re.findall(r"\[V\] retrazos retenidos (\d+) \(lanzados al esperar (\d+), por plazo (\d+), perdidos (\d+)\)", log)
    hs = [sum(int(h[i]) for h in held) for i in range(4)] if held else None
    print("%s: ventanas %s | [DEMO] start %d / fin %d | reloj %s | %s | [V] %s" % (n, win[-1] if win else "?", st, en, mhz, img[-1] if img else "imagen ?", hs))
    out = subprocess.run(["python3", "/home/djvemo/nsmbw-port/r9/r31t/tools/por_escena_real.py", b], capture_output=True, text=True).stdout
    cur = None
    for l in out.splitlines():
        if l.startswith("== "): cur = l[3:].strip()
        elif cur and l.startswith("bench_"):
            f = l.split()
            rows.setdefault(cur, []).append((n, f[1], f[2], f[4], f[6]))
print()
print("%-20s %-28s %8s %8s %10s %10s" % ("escena", "corrida", "fps dib", "fps jue", "ms jue REAL", "render ms"))
for e in ESC:
    for r in rows.get(e, []):
        print("%-20s %-28s %8s %8s %10s %10s" % (e, r[0][6:34], r[1], r[2], r[3], r[4]))
