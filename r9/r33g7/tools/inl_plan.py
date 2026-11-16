#!/usr/bin/env python3
"""inl_plan.py: que sitios de llamada directa conviene integrar, con las cuentas exactas de DX (e2).
Sitio = (funcion, ordinal del marcador PPC `// 0xADDR:` dentro de la funcion). Lee r20g6/e2/export_pre (texto antes
de G5: ahi se integra), comprueba que el llamado es integrable (reglas de apply_g6_inline.py) y escribe
diag/inl_sites.tsv: funcion, ordinal, direccion del bl, llamado, PPC del llamado, llamadas/fot (media de 5 escenas)."""
import sys, os, re, json, glob, collections
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import g6_inline_orig as G
os.environ["DXV"] = "/home/djvemo/nsmbw-port/r9/r32dx"; os.environ["DXTAG"] = "m2"
sys.path.insert(0, "/home/djvemo/nsmbw-port/r9/r33g7/dx/tools")
import dxlib
PRE = "/home/djvemo/nsmbw-port/r9/r20g6/e2/export_pre"
S = dxlib.Static()
esc = ["desierto4j", "agua_pinguino", "cielo_paratroopas", "nubes_plataformas", "castillo"]
cnt = np.zeros(len(S.call_g))
for e in esc:
    Gc, F, H = dxlib.add_segs(S, glob.glob(f"{dxlib.V}/datos/gcov60/*_{e}_main.npz"))
    fr = sum(s["xfb"] for s in dxlib.segments("gcov60") if s["seg"] == "main" and s["escena"] == e)
    cnt += np.maximum(Gc[S.call_g], 0) / fr / len(esc)
# sitio DX -> (funcion, ordinal del marcador): la instruccion PPC cuya linea precede a la linea de la llamada
order = np.argsort(S.ppc_gl)
pg = S.ppc_gl[order]
names = [f[0] for f in S.src_funcs]
first_ppc_of_fn = {}
for i in order.tolist():
    first_ppc_of_fn.setdefault(int(S.ppc_fn[i]), i)
# ordinal dentro de la funcion: posicion entre las PPC de esa funcion (estan seguidas en orden de linea)
ordn = np.zeros(len(pg), dtype=np.int64)
last_fn, k = None, 0
for j, i in enumerate(order.tolist()):
    f = int(S.ppc_fn[i])
    k = 0 if f != last_fn else k + 1
    last_fn = f; ordn[j] = k
sites = {}
for g, f, v in zip(S.call_g.tolist(), S.call_fn.tolist(), cnt.tolist()):
    if v <= 0 or f < 0: continue
    j = np.searchsorted(pg, g, side="right") - 1
    i = order[j]
    sites[(names[int(S.ppc_fn[i])], int(ordn[j]))] = (int(S.ppc_addr[i]), names[f], v)
# funciones de export_pre y su elegibilidad
funcs = {}
for x in sorted(os.listdir(PRE)):
    if not re.match(r"output_\d+\.cpp$", x): continue
    src = open(os.path.join(PRE, x)).read().split("\n")
    i = 0
    while i < len(src):
        m = G.FUNC.match(src[i])
        if m:
            j = i + 1
            while src[j] != "}": j += 1
            funcs[m.group(1)] = src[i:j + 1]; i = j + 1
        else: i += 1
elig = {}
why = collections.Counter()
for n, fn in funcs.items():
    e = G.eligible(n, G.callee_body(fn), 10 ** 9)
    elig[n] = e
tot = sum(v for _, _, v in sites.values())
rows = []
for (fn, k), (addr, callee, v) in sites.items():
    e = elig.get(callee, "missing")
    if isinstance(e, tuple): rows.append((v, fn, k, addr, callee, e[1]))
    else: why[e] += v
rows.sort(reverse=True)
print(f"llamadas directas/fot: {tot:,.0f} en {len(sites)} sitios; a llamados integrables: {sum(r[0] for r in rows):,.0f}; no integrables: " + ", ".join(f"{k} {v:,.0f}" for k, v in why.most_common()))
for maxppc in (64, 128, 200, 400):
    for minc in (20, 5, 2, 1):
        sel = [r for r in rows if r[5] <= maxppc and r[0] >= minc]
        print(f"  PPC<={maxppc:4d} y >= {minc:2d} llamadas/fot: {len(sel):5d} sitios, {sum(r[0] for r in sel):8,.0f} llamadas/fot ({100*sum(r[0] for r in sel)/tot:4.1f} %), +{sum(r[5] for r in sel):7,d} PPC copiadas")
with open("/home/djvemo/nsmbw-port/r9/r33g7/diag/inl_sites.tsv", "w") as f:
    for v, fn, k, addr, callee, ppc in rows:
        f.write(f"{fn}\t{k}\t{addr:08X}\t{callee}\t{ppc}\t{v:.3f}\n")
