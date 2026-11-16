#!/usr/bin/env python3
"""dx_huella.py <corrida con NWII_DX_PERFRAME> [K]: huella de código AArch64 (m2) POR FOTOGRAMA.

Usa los segmentos '<escena>_f' (uno por periodo de VI = un fotograma a 60 fot/s) ya pasados por gcov_seg.py
(los K primeros de cada escena, 8 por defecto). Por escena imprime la media por fotograma de:
  bytes de código ejecutado, líneas de caché de 64 B y páginas de 4 KB distintas tocadas, funciones distintas,
  bytes que concentran el 50/90/99 % de las instrucciones ejecutadas, cuánto se repite de un fotograma al
  siguiente, y la unión de los K fotogramas. Referencia Cortex-A57: L1I 48 KB (750 líneas), L2 2 MB (compartida
  por los 4 núcleos, código + datos), TLB de instrucciones L1 48 entradas (192 KB con páginas de 4 KB).
"""
import collections
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxlib  # noqa: E402
from dxlib import V  # noqa: E402

run = sys.argv[1]
K = int(sys.argv[2]) if len(sys.argv) > 2 else 8
S = dxlib.Static()
segs = [s for s in dxlib.segments(run) if s["seg"] == "f"]
by = collections.OrderedDict()
for s in segs:
    by.setdefault(s["escena"], []).append(s)
out = {}
print(f"{'escena':18s} {'fot':>3s} {'A64/fot':>10s} {'KB ejec':>8s} {'líneas64':>8s} {'KB líneas':>9s} {'págs4k':>6s} {'func':>5s} "
      f"{'KB 50%':>6s} {'KB 90%':>6s} {'KB 99%':>6s} {'repite %':>8s} {'unión KB':>8s} {'siempre KB':>10s}")
for esc, ss in by.items():
    rows, prev, uni, inter = [], None, None, None
    for s in ss[:K]:
        p = f"{V}/datos/{run}/{s['n']:02d}_{esc}_f.npz"
        if not os.path.exists(p):
            continue
        G, F, H = dxlib.load_seg(S, p)
        med = dxlib.a64_counts(S, G, H)[0]
        ex = med > 0
        addr = np.flatnonzero(ex).astype(np.int64) * 4
        lines = np.unique(addr >> 6)
        w = np.sort(med[ex])[::-1].astype(np.float64)
        cum = np.cumsum(w)
        q = [int(np.searchsorted(cum, cum[-1] * x / 100.0) + 1) * 4 / 1024 for x in (50, 90, 99)] if len(cum) else [0, 0, 0]
        rep = 100.0 * len(np.intersect1d(lines, prev)) / len(lines) if prev is not None and len(lines) else float("nan")
        rows.append((float(med.sum()), int(ex.sum()) * 4 / 1024, len(lines), len(np.unique(addr >> 12)), len(F), q[0], q[1], q[2], rep))
        prev = lines
        uni = lines if uni is None else np.union1d(uni, lines)
        inter = lines if inter is None else np.intersect1d(inter, lines)
    if not rows:
        continue
    a = np.array(rows, dtype=np.float64)
    m = np.nanmean(a, axis=0)
    print(f"{esc:18s} {len(rows):3d} {m[0]:10,.0f} {m[1]:8,.0f} {m[2]:8,.0f} {m[2] * 64 / 1024:9,.0f} {m[3]:6,.0f} {m[4]:5,.0f} "
          f"{m[5]:6,.0f} {m[6]:6,.0f} {m[7]:6,.0f} {m[8]:8.1f} {len(uni) * 64 / 1024:8,.0f} {len(inter) * 64 / 1024:10,.0f}")
    out[esc] = {"n": len(rows), "a64": m[0], "kb_ejec": m[1], "lineas": m[2], "paginas": m[3], "funciones": m[4],
                "kb50": m[5], "kb90": m[6], "kb99": m[7], "repite_pct": m[8], "union_kb": len(uni) * 64 / 1024,
                "siempre_kb": len(inter) * 64 / 1024}
json.dump(out, open(f"{V}/datos/{run}/huella.json", "w"), indent=1)
