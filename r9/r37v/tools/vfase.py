#!/usr/bin/env python3
"""vfase.py <bench con tprof> <nm> [escena]: por fotograma del juego (entre copias XFB), fin del trabajo
(ultima muestra ocupada) y duracion; fase de cada copia XFB respecto a una rejilla de 16,667 ms."""
import sys, os, collections
sys.path.insert(0, "/home/djvemo/nsmbw-port/r9/r35m4/tools")
sys.argv_save = list(sys.argv)
import tprof_m4 as T
d, nm = sys.argv[1], sys.argv[2]
esc = sys.argv[3] if len(sys.argv) > 3 else "agua_pinguino"
pr = T.Prof(d, nm, T.P + "/game/SMNE01/sys/main.dol", T.P + "/symbols/usa_symbols.txt")
fasesf = "/home/djvemo/nsmbw-port/r9/r35m4/tools/fases.py"
ns = {}; exec(open(fasesf).read(), ns); T.NOMBRES.update(ns.get("NOMBRES", {}))
recs = [r for r in pr.recs if T.scene_name(r[8]) == esc]
t0s = recs[0][0]
recs = [r for r in recs if r[0] - t0s > 5_000_000]
fr = []; p = None; ts = None; last_busy = None; prev = None
for r in recs:
    if prev is not None and r[0] - prev > 50000: p = None
    prev = r[0]
    if p is None or r[7] != p:
        if p is not None and ((r[7] - p) & 0xFFFF) == 1:
            fr.append((ts, r[0] - ts, (last_busy - ts) if last_busy else 0))
        p = r[7]; ts = r[0]; last_busy = None
    if r[9] & 1 and not T.is_idle(pr, pr.sample_chain(r), r):
        last_busy = r[0]
print("%s: %d fotogramas" % (esc, len(fr)))
B = [0, 12, 14, 15, 16, 16.7, 18, 25, 1e9]
tab = collections.defaultdict(collections.Counter)
for ts, w, b in fr:
    bb = next(i for i in range(len(B) - 1) if B[i] <= b / 1000 < B[i + 1])
    tab[bb]["n"] += 1
    tab[bb]["2r" if w > 25000 else "1r"] += 1
print("fin del trabajo (ms desde la copia XFB) -> fotogramas de 1 / 2+ retrazos")
for i in range(len(B) - 1):
    c = tab[i]
    if c["n"]:
        print("  %5.1f-%5.1f: %5d  1r %5.1f%%  2r+ %5.1f%%" % (B[i], B[i + 1], c["n"], 100 * c["1r"] / c["n"], 100 * c["2r"] / c["n"]))
# fase de las copias XFB en rejilla de 16,667 ms
ph = collections.Counter()
for ts, w, b in fr:
    ph[int(((ts / 1000.0) % 16.6667) // 1)] += 1
print("fase de la copia XFB (ms mod 16,667):", " ".join("%d:%d" % (k, ph[k]) for k in sorted(ph)))
# secuencia de ejemplo
print("ejemplo (inicio ms, pared ms, fin trabajo ms):")
for ts, w, b in fr[300:340]:
    print("  %10.1f %6.1f %6.1f" % (ts / 1000.0, w / 1000.0, b / 1000.0))
