#!/usr/bin/env python3
"""perf_x86.py <escena> [seg=20] [fps=60]: resumen de runs/perf_<escena>/sym.txt (tools/perf_x86.sh), hilo de la CPU."""
import collections
import re
import sys

V = "/home/djvemo/nsmbw-port/r9/r32dx"
E = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
fps = float(sys.argv[3]) if len(sys.argv) > 3 else 60.0
GEN = re.compile(r"(^func_|_8[0-9A-Fa-f]{7}$)")
PER = {"instructions": 2000003, "L1-icache-load-misses": 4001, "cache-misses": 1009, "branch-misses": 1009}
by = collections.defaultdict(lambda: collections.Counter())
for ln in open(f"{V}/runs/perf_{E}/sym.txt"):
    p = ln.split(None, 2)
    if len(p) < 2:
        continue
    tid, ev = p[0], p[1].rstrip(":")
    sym = p[2].strip() if len(p) > 2 else "?"
    ev = next((k for k in sorted(PER, key=len, reverse=True) if "/" + k + "," in ev or "/" + k + "/" in ev), None)
    if ev:
        by[(tid, ev)][sym] += 1
# hilo de la CPU = el que tiene más muestras de instrucciones en código generado
best = max({t for t, _ in by}, key=lambda t: sum(v for k, v in by[(t, "instructions")].items() if GEN.search(k)))
fr = secs * fps
print(f"{E}: hilo {best}, {fr:.0f} fotogramas")
for ev, per in PER.items():
    c = by[(best, ev)]
    tot = sum(c.values())
    gen = sum(v for k, v in c.items() if GEN.search(k) and k != "func_801B4EA0")
    idle = sum(v for k, v in c.items() if k == "func_801B4EA0" or "idle_wait" in k or "clock_gettime" in k or "nanosleep" in k
               or "wall_timebase_now" in k or "chrono" in k)
    print(f"  {ev:24s} total {tot * per / fr:12,.0f}/fot   código generado {gen * per / fr:12,.0f}   ocioso+reloj {idle * per / fr:12,.0f}   "
          f"resto (runtime) {(tot - gen - idle) * per / fr:12,.0f}")
    if ev == "instructions":
        print("    no generado más caro: " + ", ".join(f"{k[:40]} {v * per / fr:,.0f}" for k, v in c.most_common(40)
                                                       if not GEN.search(k))[:600])
