#!/usr/bin/env python3
"""dx_static_cls.py <escena>: bytes de codigo (m2/e2) por clase de sentencia, en funciones ejecutadas: ejecutado / no ejecutado."""
import sys, glob
import numpy as np
sys.path.insert(0, "/home/djvemo/nsmbw-port/r9/r32dx/tools")
import dxlib
S = dxlib.Static()
esc = sys.argv[1]
G, F, H = dxlib.add_segs(S, glob.glob(f"{dxlib.V}/datos/gcov60/*_{esc}_main.npz"))
med, lo, hi, craw, flujo = dxlib.a64_counts(S, G, H, {})
fex = np.zeros(len(S.funcsA) + 1, dtype=bool)
fex[np.unique(S.fnA[med > 0])] = True
infn = fex[S.fnA] & S.gen
ex = med > 0
ncl = len(S.clasesS)
cls = np.full(S.n, ncl + 1, dtype=np.int64)       # sin linea
own = S.gidx >= 0
cls[own] = S.G_cls[S.gidx[own]]
cls[S.hdr] = ncl
names = S.clasesS + ["cabecera", "sin linea"]
tot = infn.sum()
print(f"{esc}: funciones ejecutadas {fex.sum()}, {tot*4/1024:.0f} KB; ejecutado {(infn&ex).sum()*4/1024:.0f} KB")
a = np.bincount(cls[infn & ex], minlength=ncl + 2); b = np.bincount(cls[infn & ~ex], minlength=ncl + 2)
w = np.bincount(cls[infn], weights=med[infn].astype(float), minlength=ncl + 2)
for i in np.argsort(-(a + b)):
    if a[i] + b[i]:
        print(f"  {names[i]:16s} ejecutado {a[i]*4/1024:7.0f} KB ({100*a[i]/max((infn&ex).sum(),1):4.1f}%)  no ejecutado {b[i]*4/1024:7.0f} KB ({100*b[i]/max((infn&~ex).sum(),1):4.1f}%)   dinamico {100*w[i]/w.sum():4.1f}%")
# cabecera por linea: estatico no ejecutado
m = infn & S.hdr
key = (S.hid[m].astype(np.int64) << 24) | S.hlin[m].astype(np.int64)
uk, inv = np.unique(key, return_inverse=True)
se = np.bincount(inv, weights=ex[m].astype(float)); sn = np.bincount(inv, weights=(~ex[m]).astype(float))
print("  cabecera por linea (KB ejecutado / no ejecutado):")
for i in np.argsort(-(se + sn))[:40]:
    print(f"    {S.cab[uk[i]>>24]}:{uk[i]&0xFFFFFF:<6d} {se[i]*4/1024:7.0f} {sn[i]*4/1024:7.0f}")
