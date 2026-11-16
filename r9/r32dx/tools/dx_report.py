#!/usr/bin/env python3
"""dx_report.py <corrida> [--top N] [--csv]: tablas por escena de una corrida de NSMBW_dxgcov ya pasada por gcov_seg.py.

Por cada segmento 'main' de cada escena (ventana estable: sin la carga ni la cola):
  - fotogramas (copias XFB), instrucciones PPC exactas por fotograma (y en ciclos de Dolphin), inst_count/fot
  - instrucciones AArch64 del código generado de m2 por fotograma (estimación media y cotas), ratio AArch64/PPC
  - reparto por clase de instrucción AArch64 y por clase de sentencia del fuente (protocolo frente a trabajo)
  - llamadas directas, despachos indirectos, entradas a función, sondeos por fotograma
  - huella de código: bytes / líneas de caché / páginas distintas ejecutadas; bytes que cubren el 90 % y el 99 %
  - las N funciones con más instrucciones AArch64 por fotograma
Escribe además datos/<corrida>/resumen.json (lo usa tools/dx_ipc.py).
"""
import argparse
import collections
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dxlib  # noqa: E402
from dxlib import V  # noqa: E402

PROTO_A = ("ctx.gpr", "ctx.cr", "ctx.pc", "ctx.lr", "ctx.ctr", "ctx.xer", "ctx.inst_count", "ctx.mmu", "ctx.poll",
           "ctx.msr", "ctx.pending", "ctx.other", "ctx.srr", "ctx.fpscr", "ctx.gqr")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("run")
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--seg", default="main")
    a = ap.parse_args()
    S = dxlib.Static()
    segs = [s for s in dxlib.segments(a.run) if s["seg"] == a.seg and s["escena"] != "entre"]
    by = collections.OrderedDict()
    for s in segs:
        by.setdefault(s["escena"], []).append(s)
    out = {}
    fnameA = [f[0] for f in S.funcsA]
    for esc, ss in by.items():
        paths = [f"{V}/datos/{a.run}/{s['n']:02d}_{esc}_{a.seg}.npz" for s in ss]
        paths = [p for p in paths if os.path.exists(p)]
        if not paths:
            continue
        ss = [s for s in ss if os.path.exists(f"{V}/datos/{a.run}/{s['n']:02d}_{esc}_{a.seg}.npz")]
        fr = sum(s["xfb"] for s in ss)
        vi = sum(s["vi"] for s in ss)
        wall = sum(s["t"] for s in ss)
        G, F, H = dxlib.add_segs(S, paths)
        pc, pok = dxlib.ppc_counts(S, G)
        ppc = int(pc.sum())
        cyc = int((pc * S.ppc_cyc).sum())
        dg = {}
        med, lo, hi, craw, flujo = dxlib.a64_counts(S, G, H, dg)
        A = int(med.sum())
        r = {"escena": esc, "fotogramas": fr, "vi": vi, "fps_pc": fr / wall if wall else 0,
             "inst_count_fot": sum(s["inst"] for s in ss) / fr, "ppc_fot": ppc / fr, "ppc_dolcyc_fot": cyc / fr,
             "ppc_sin_sentencia_pct": 100.0 * float(pc[~pok].sum()) / max(ppc, 1),
             "a64_fot": A / fr, "a64_lo_fot": int(lo.sum()) / fr, "a64_hi_fot": int(hi.sum()) / fr,
             "cpu_ms_pc_fot": 1000.0 * sum(s["cpu_s"] for s in ss) / fr,
             "x86_ins_fot": sum(s.get("x86_ins", 0) for s in ss) / fr, "x86_cyc_fot": sum(s.get("x86_cyc", 0) for s in ss) / fr}
        # estimación "por línea": cada instrucción con línea propia usa su cuenta; el resto, la del bloque
        direct = np.where(craw >= 0, craw, med)
        direct[~S.gen] = 0
        r["a64_linea_fot"] = int(direct.sum()) / fr
        r["a64_flujo_fot"] = int(flujo.sum()) / fr
        r["diag"] = dg
        r["a64_en_cabeceras_pct"] = 100.0 * int(med[S.hdr].sum()) / max(A, 1)
        # clases AArch64
        ca = np.bincount(S.cls, weights=med.astype(np.float64), minlength=len(S.clasesA))
        r["clases_a64"] = {S.clasesA[i]: ca[i] / fr for i in np.argsort(-ca) if ca[i] > 0}
        # clases de sentencia (instrucciones con línea propia); las de cabecera aparte
        own = S.gidx >= 0
        cs = np.bincount(S.G_cls[S.gidx[own]], weights=med[own].astype(np.float64), minlength=len(S.clasesS))
        d = {S.clasesS[i]: cs[i] / fr for i in range(len(S.clasesS)) if cs[i] > 0}
        d["cabecera (integrado: memoria, fp, psq, sondeo)"] = int(med[S.hdr].sum()) / fr
        r["clases_fuente"] = d
        hb = np.bincount(S.hid[S.hdr], weights=med[S.hdr].astype(np.float64), minlength=len(S.cab))
        r["cabeceras"] = {S.cab[i]: hb[i] / fr for i in np.argsort(-hb)[:6] if hb[i] > 0}
        # tamaño total de las funciones ejecutadas (para ver cuánto código frío va mezclado con el caliente)
        fex = np.unique(S.fnA[(med > 0)])
        r["kb_funciones_ejecutadas"] = sum(S.funcsA[i][2] for i in fex.tolist() if i >= 0) / 1024
        # cuentas de sentencias (exactas)
        gc = np.maximum(G, 0)
        r["llamadas_directas_fot"] = int(gc[S.call_g].sum()) / fr
        r["despachos_fot"] = int(gc[S.disp_g].sum()) / fr
        r["entradas_funcion_fot"] = sum(F.values()) / fr
        # sondeos: la línea del `if (poll_due)` no queda instrumentada (su condición es código de cabecera);
        # se cuenta la sentencia instrumentada anterior (`ctx.pc = X;`, hasta 3 líneas antes)
        idle_i = next((i for i, f in enumerate(S.src_funcs) if f[0] == "func_801B4EA0"), -2)
        pl = np.flatnonzero((S.G_cls == S.clasesS.index("sondeo")) & (S.G_fn != idle_i))   # sin el bucle ocioso
        pcnt = np.zeros(len(pl), dtype=np.int64)
        done = np.zeros(len(pl), dtype=bool)
        for d_ in (0, 1, 2, 3):
            v_ = G[pl - d_]
            m_ = ~done & (v_ >= 0)
            pcnt[m_] = v_[m_]
            done |= m_
        r["sondeos_fot"] = int(pcnt.sum()) / fr
        slow = np.flatnonzero(S.tok_id == S.toks.index("process_pending_callbacks_slow"))
        slow = slow[S.G_fn[S.tok_g[slow]] != idle_i]
        r["sondeos_lentos_fot"] = int(gc[S.tok_g[slow]].sum()) / fr
        tk = np.bincount(S.tok_id, weights=gc[S.tok_g].astype(np.float64), minlength=len(S.toks))
        r["runtime_fot"] = {S.toks[i]: tk[i] / fr for i in range(len(S.toks)) if tk[i] > 0}
        # huella
        exm = med > 0
        addr = np.flatnonzero(exm).astype(np.int64) * 4
        r["huella_bytes"] = int(exm.sum()) * 4
        r["huella_lineas64"] = int(len(np.unique(addr >> 6)))
        r["huella_paginas4k"] = int(len(np.unique(addr >> 12)))
        r["funciones_ejecutadas"] = len(F)
        w = np.sort(med[exm])[::-1].astype(np.float64)
        cum = np.cumsum(w)
        for q in (50, 90, 99):
            r[f"bytes_{q}pct"] = int(np.searchsorted(cum, cum[-1] * q / 100.0) + 1) * 4
        # por función AArch64
        fa = np.bincount(np.maximum(S.fnA, 0), weights=med.astype(np.float64), minlength=len(S.funcsA))
        # PPC por función del fuente -> función AArch64
        fp = np.bincount(np.maximum(S.ppc_fn, 0), weights=pc.astype(np.float64), minlength=len(S.src_funcs))
        fpa = np.zeros(len(S.funcsA))
        okm = S.s2a >= 0
        np.add.at(fpa, S.s2a[okm], fp[okm])
        # trabajo frente a protocolo por función (clases de sentencia + pila)
        work_cls = np.zeros(len(S.clasesS), dtype=bool)
        work_cls[S.clasesS.index("trabajo")] = True
        isw = np.zeros(S.n, dtype=bool)
        isw[own] = work_cls[S.G_cls[S.gidx[own]]]
        isw |= S.hdr
        stack = np.isin(S.cls, [S.clasesA.index("stack ld"), S.clasesA.index("stack st")])
        fw = np.bincount(np.maximum(S.fnA, 0), weights=(med * (isw & ~stack)).astype(np.float64), minlength=len(S.funcsA))
        r["trabajo_pct"] = 100.0 * float(fw.sum()) / max(A, 1)
        top = []
        for i in np.argsort(-fa)[:400]:
            if fa[i] <= 0:
                break
            nm = fnameA[i]
            top.append({"f": nm, "a64_fot": fa[i] / fr, "ppc_fot": fpa[i] / fr, "llamadas_fot": F.get(nm, 0) / fr,
                        "trabajo_pct": 100.0 * fw[i] / fa[i]})
        r["top"] = top
        # por biblioteca (dirección invitada del nombre)
        libs = collections.Counter()
        libp = collections.Counter()
        for i in np.flatnonzero(fa > 0):
            nm = fnameA[i]
            try:
                ad = int(nm[-8:], 16)
            except ValueError:
                continue
            libs[dxlib.lib_of(ad)] += fa[i] / fr
            libp[dxlib.lib_of(ad)] += fpa[i] / fr
        r["bibliotecas"] = {k: [libs[k], libp[k]] for k in libs}
        out[esc] = r
        print(f"\n===== {esc}: {fr} fotogramas ({r['fps_pc']:.1f} fps en PC, {vi / fr:.3f} VI/fot)")
        print(f"  PPC exactas/fot {r['ppc_fot']:,.0f}   (ciclos Dolphin {r['ppc_dolcyc_fot']:,.0f}; inst_count {r['inst_count_fot']:,.0f}; "
              f"sin sentencia propia {r['ppc_sin_sentencia_pct']:.2f} %)")
        print(f"  AArch64 generadas/fot {r['a64_fot']:,.0f}  [cotas {r['a64_lo_fot']:,.0f} .. {r['a64_hi_fot']:,.0f}; por flujo {r['a64_flujo_fot']:,.0f}]"
              f"   ratio {r['a64_fot'] / r['ppc_fot']:.2f} AArch64/PPC   (en código de cabeceras {r['a64_en_cabeceras_pct']:.1f} %)")
        if dg:
            print(f"  calibración de bloques sin línea propia: {100 * dg['peso_alto_calibrado'] / max(dg['peso_alto_bloques_sin_linea'], 1):.0f} % del peso con dato; "
                  f"r medio {dg['peso_cal'] / max(dg['peso_alto_bloques_sin_linea'], 1):.2f}; control (líneas de cabecera solo en bloques con línea): "
                  f"gcov {dg['cab_solo_conocidos_gcov'] / fr:,.0f} frente a predicho {dg['cab_solo_conocidos_pred'] / fr:,.0f} por fot")
        print(f"  x86 (hilo entero, PC): {r['x86_ins_fot']:,.0f} ins/fot, {r['x86_cyc_fot']:,.0f} ciclos/fot, {r['cpu_ms_pc_fot']:.2f} ms/fot")
        print(f"  por fot: llamadas directas {r['llamadas_directas_fot']:,.0f}, despachos indirectos {r['despachos_fot']:,.0f}, "
              f"entradas a función {r['entradas_funcion_fot']:,.0f}, sondeos {r['sondeos_fot']:,.0f} (lentos {r['sondeos_lentos_fot']:,.0f})")
        print("  usos por fot de funciones del runtime: " + ", ".join(f"{k} {v:,.0f}" for k, v in sorted(r["runtime_fot"].items(), key=lambda kv: -kv[1])))
        print(f"  huella de código ejecutado: {r['huella_bytes'] / 1024:,.0f} KB en {r['huella_lineas64']:,} líneas de 64 B y "
              f"{r['huella_paginas4k']:,} páginas; {r['funciones_ejecutadas']} funciones; 50/90/99 % del trabajo en "
              f"{r['bytes_50pct'] / 1024:,.0f} / {r['bytes_90pct'] / 1024:,.0f} / {r['bytes_99pct'] / 1024:,.0f} KB")
        print("  instrucciones de código integrado por cabecera (por fot): " + ", ".join(f"{k} {v:,.0f}" for k, v in r["cabeceras"].items())
              + f"   | tamaño total de las funciones ejecutadas: {r['kb_funciones_ejecutadas']:,.0f} KB")
        print("  clases AArch64 (% de las ejecutadas):")
        tot = sum(r["clases_a64"].values())
        print("   " + ", ".join(f"{k} {100 * v / tot:.1f}" for k, v in list(r["clases_a64"].items())[:22]))
        print("  clases de sentencia (% de las AArch64 ejecutadas):")
        tot = sum(r["clases_fuente"].values())
        print("   " + ", ".join(f"{k} {100 * v / tot:.1f}" for k, v in sorted(r["clases_fuente"].items(), key=lambda kv: -kv[1])))
        print(f"  trabajo (sentencias de la instrucción PPC + código integrado, sin pila): {r['trabajo_pct']:.1f} %")
        print("  bibliotecas (AArch64/fot, PPC/fot, ratio): " + ", ".join(
            f"{k} {v[0]:,.0f}/{v[1]:,.0f}/{v[0] / max(v[1], 1):.1f}" for k, v in sorted(r["bibliotecas"].items(), key=lambda kv: -kv[1][0])))
        print(f"  top {a.top} funciones: AArch64/fot, % del total, PPC/fot, ratio, llamadas/fot, AArch64 por llamada, % trabajo")
        for t in top[:a.top]:
            print(f"   {t['a64_fot']:10,.0f} {100 * t['a64_fot'] / r['a64_fot']:5.2f}% {t['ppc_fot']:9,.0f} "
                  f"{t['a64_fot'] / max(t['ppc_fot'], 1):5.2f} {t['llamadas_fot']:9.1f} "
                  f"{t['a64_fot'] / max(t['llamadas_fot'], 1e-9):8.0f} {t['trabajo_pct']:5.1f}  {t['f']}")
    json.dump(out, open(f"{V}/datos/{a.run}/resumen_{a.seg}.json", "w"), indent=1)


if __name__ == "__main__":
    main()
