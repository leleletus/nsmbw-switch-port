#!/usr/bin/env python3
"""dx_calls.py: con las cuentas exactas de DX (r32dx, e2): llamadas directas por fotograma por arista (sitio) y por
llamado, con el tamano del llamado en instrucciones PPC. Para elegir que integrar (inline) en el llamador."""
import sys, glob, json, collections
import numpy as np, os
os.environ["DXV"] = "/home/djvemo/nsmbw-port/r9/r32dx"; os.environ["DXTAG"] = "m2"
sys.path.insert(0, "/home/djvemo/nsmbw-port/r9/r33g7/dx/tools")
import dxlib
S = dxlib.Static()
tot_by_callee = collections.Counter(); site = collections.Counter(); tot = 0; frames = {}
esc = sys.argv[1:] or ["desierto4j", "agua_pinguino", "cielo_paratroopas", "nubes_plataformas", "castillo"]
for e in esc:
    G, F, H = dxlib.add_segs(S, glob.glob(f"{dxlib.V}/datos/gcov60/*_{e}_main.npz"))
    fr = sum(s["xfb"] for s in dxlib.segments("gcov60") if s["seg"] == "main" and s["escena"] == e)
    c = np.maximum(G[S.call_g], 0) / fr / len(esc)
    for g, f, v in zip(S.call_g.tolist(), S.call_fn.tolist(), c.tolist()):
        if v > 0:
            tot_by_callee[f] += v; site[(g, f)] += v; tot += v
names = [f[0] for f in S.src_funcs]; nppc = [f[4] for f in S.src_funcs]
print(f"llamadas directas por fotograma (media de {len(esc)} escenas): {tot:,.0f}; sitios con llamadas: {len(site)}")
for lim in (16, 32, 48, 64, 96, 128, 200, 400, 100000):
    v = sum(x for f, x in tot_by_callee.items() if f >= 0 and nppc[f] <= lim)
    print(f"  llamados de <= {lim:6d} PPC: {v:9,.0f} llamadas/fot ({100*v/tot:5.1f} %)")
print("llamados mas llamados: llamadas/fot, PPC estatico, nombre, n sitios activos")
ns = collections.Counter(f for (g, f) in site)
acc = 0
for f, v in tot_by_callee.most_common(45):
    acc += v
    print(f"  {v:8.1f} {100*acc/tot:5.1f}%  {nppc[f] if f>=0 else -1:5d}  {names[f] if f>=0 else '?'}  ({ns[f]} sitios)")
json.dump({"sites": [[int(g), names[f] if f >= 0 else "?", v] for (g, f), v in site.most_common()]}, open("/home/djvemo/nsmbw-port/r9/r33g7/diag/dx_call_sites.json", "w"))
vals = sorted(site.values(), reverse=True); cum = np.cumsum(vals)
for k in (100, 300, 1000, 3000, 10000):
    if k <= len(vals): print(f"  los {k} sitios mas calientes: {100*cum[k-1]/tot:.1f} % de las llamadas")
