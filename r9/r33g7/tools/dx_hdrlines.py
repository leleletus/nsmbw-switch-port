#!/usr/bin/env python3
"""dx_hdrlines.py <escena-npz-substr>: instrucciones AArch64 ejecutadas (estimacion DX, m2/e2) por linea de cabecera."""
import sys, os, glob, collections
import numpy as np
sys.path.insert(0, "/home/djvemo/nsmbw-port/r9/r32dx/tools")
import dxlib
S = dxlib.Static()
for esc in sys.argv[1:]:
    paths = glob.glob(f"{dxlib.V}/datos/gcov60/*_{esc}_main.npz")
    G, F, H = dxlib.add_segs(S, paths)
    med, lo, hi, craw, flujo = dxlib.a64_counts(S, G, H, {})
    tot = med.sum()
    segs = [s for s in dxlib.segments("gcov60") if s["seg"] == "main" and s["escena"] == esc]
    fr = sum(s["xfb"] for s in segs)
    m = S.hdr
    key = (S.hid[m].astype(np.int64) << 24) | S.hlin[m].astype(np.int64)
    uk, inv = np.unique(key, return_inverse=True)
    w = np.bincount(inv, weights=med[m].astype(np.float64))
    st = np.bincount(inv)
    print(f"== {esc}: total {tot/fr:,.0f} A64/fot; cabeceras {med[m].sum()/fr:,.0f} ({100*med[m].sum()/tot:.1f}%)")
    o = np.argsort(-w)
    for i in o[:70]:
        print(f"  {w[i]/fr:10,.0f} {100*w[i]/tot:5.2f}%  est {st[i]:8d}  {S.cab[uk[i]>>24]}:{uk[i]&0xFFFFFF}")
