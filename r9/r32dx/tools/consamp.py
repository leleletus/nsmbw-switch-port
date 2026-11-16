#!/usr/bin/env python3
"""consamp.py [carpeta del bench con tprof] [nm]: muestras del hilo del juego en la CONSOLA por escena y por función.

Lee (solo lectura) las muestras con marca de escena y de fotograma del perfil del frente T
(switch/real_logs/bench_T01_tp1_tprof_1112_0330, binario tp1 = mismo código generado y mismo orden que m2) y
deja datos/consamp_T01.json: por escena -> fotogramas del juego, ms de pared, ms por muestra y muestras por
símbolo del anfitrión. Mismas reglas que r31t/tools/tprof.py: se saltan los 5 primeros segundos de cada escena;
fotogramas y pared solo entre muestras seguidas (< 50 ms).
"""
import bisect
import collections
import json
import os
import struct
import sys

P = "/home/djvemo/nsmbw-port"
d = sys.argv[1] if len(sys.argv) > 1 else P + "/switch/real_logs/bench_T01_tp1_tprof_1112_0330"
nm = sys.argv[2] if len(sys.argv) > 2 else P + "/r9/r31t/datos/tp1.nm"
DEMOS = ["agua_pinguino", "pradera", "desierto4j", "desierto_helices", "nieve", "nubes_plataformas", "torre3",
         "playa_hielo", "playa_yoshi", "jungla", "cielo_hoja", "casa_oscura", "castillo6", "cielo_paratroopas", "lava", "castillo"]
syms = []
for ln in open(nm):
    q = ln.split()
    if len(q) == 4 and q[2] in "tTwW":
        syms.append((int(q[0], 16), int(q[1], 16), q[3]))
    elif len(q) == 3 and q[1] in "tTwW":
        syms.append((int(q[0], 16), 0, q[2]))
syms.sort()
st = [s[0] for s in syms]


def sym(o):
    i = bisect.bisect_right(st, o) - 1
    if i >= 0 and (o < syms[i][0] + syms[i][1] or (syms[i][1] == 0 and (i + 1 == len(syms) or o < syms[i + 1][0]))):
        return syms[i][2]
    return "?"


raw = open(os.path.join(d, "tprof_samples.bin"), "rb").read()
recs = list(struct.iter_unpack("<IIIIIIIHbB", raw[:len(raw) // 32 * 32]))
dts = sorted(recs[i + 1][0] - recs[i][0] for i in range(min(len(recs) - 1, 20000)))
us = dts[len(dts) // 2]
by, start, prev = collections.defaultdict(list), {}, None
for r in recs:
    s = r[8]
    if s != prev:
        start[s] = r[0]
        prev = s
    if r[0] - start[s] >= 5_000_000:
        by[s].append(r)
out = {}
for s, rs in by.items():
    if not (0 <= s < len(DEMOS)):
        continue
    fr = wall = 0
    for i in range(1, len(rs)):
        dt = rs[i][0] - rs[i - 1][0]
        if 0 < dt < 50000:
            fr += (rs[i][7] - rs[i - 1][7]) & 0xFFFF
            wall += dt
    c = collections.Counter()
    for r in rs:
        c[sym(r[1]) if r[1] != 0xFFFFFFFF else "?"] += 1
    out[DEMOS[s]] = {"fotogramas": fr, "pared_ms": wall / 1000.0, "us_muestra": us, "muestras": len(rs), "por_simbolo": dict(c)}
    print(f"{DEMOS[s]:20s} {len(rs):7d} muestras, {fr:5d} fotogramas, {wall / 1000.0 / max(fr, 1):6.2f} ms de pared por fotograma")
json.dump(out, open(P + "/r9/r32dx/datos/consamp_T01.json", "w"))
