#!/usr/bin/env python3
"""dxlib.py (r32dx): une las cuentas exactas de gcov (PC, por línea del fuente partido) con el código AArch64 de m2.

Piezas (todas en datos/):
  srcmap/N.npz, funcs.json     tools/srcmap.py      clase de cada línea, instrucciones PPC, funciones
  a64/m2_static.npz            tools/a64_static.py  clase / bloque básico / función de cada instrucción AArch64 de m2
  a64/m2_lines.npz             tools/a64_lines.py   línea de fuente de cada instrucción AArch64 (ELF con -g1, .text idéntico)
  <corrida>/<seg>.npz          tools/gcov_seg.py    veces que se ejecutó cada línea en ese segmento (escena)

Método para las instrucciones AArch64 ejecutadas: cada instrucción de un bloque básico se ejecuta las mismas veces
que el bloque. La cuenta del bloque = mediana de las cuentas (exactas) de las líneas de output_N.cpp de sus
instrucciones. Bloques hechos solo de código integrado de cabeceras (sin línea propia): por conservación de flujo
con sus predecesores (estimación "media"), con cotas baja (0) y alta (hereda del bloque anterior).
"""
import json
import os
import re

import numpy as np

V = "/home/djvemo/nsmbw-port/r9/r32dx"
GEN = re.compile(r"(^func_|_8[0-9A-Fa-f]{7}$)")


# Subsistema por dirección invitada (USA): la tabla SUBSIS del frente T (r9/r31t/tools/fases.py), copiada.
SUBSIS = [
    (0x80004000, 0x80006780, "MSL/rt"), (0x80006780, 0x80046000, "juego: resto"), (0x80046000, 0x80062000, "juego: jugador"),
    (0x80062000, 0x8006CF40, "juego: resto"), (0x8006CF40, 0x8008C200, "juego: colision fondo (dBc/dBg)"),
    (0x8008C200, 0x8008D8B0, "juego: colision actores (dCc)"), (0x8008D8B0, 0x80095000, "juego: resto"),
    (0x80095000, 0x800A8300, "juego: enemigos (dEn)"), (0x800A8300, 0x800D5000, "juego: resto"),
    (0x800D5000, 0x800D8600, "juego: jugador"), (0x800D8600, 0x80126000, "juego: resto"), (0x80126000, 0x8014E000, "juego: jugador"),
    (0x8014E000, 0x80160000, "juego: resto"), (0x80160000, 0x80164000, "juego: framework (fBase, listas)"),
    (0x80164000, 0x80165000, "nw4r::lyt + m2d"), (0x80165000, 0x8016B000, "nw4r::g3d + m3d"), (0x8016B000, 0x80177000, "juego: resto"),
    (0x80177000, 0x80195000, "HBM"), (0x80195000, 0x801A0000, "sonido: Snd* del juego"),
    (0x801A0000, 0x801ABA00, "sonido: AX/AXFX/AI/DSP (SDK)"), (0x801ABA00, 0x801BC000, "SDK: OS"),
    (0x801BC000, 0x801C0400, "SDK: VI/EXI/SI"), (0x801C0400, 0x801C3000, "SDK: MTX"), (0x801C3000, 0x801CAD00, "SDK: GX"),
    (0x801CAD00, 0x80228D00, "SDK: DVD/MEM/WPAD/KPAD..."), (0x80228D00, 0x80237B00, "nw4r::ut"), (0x80237B00, 0x8023A000, "nw4r::math"),
    (0x8023A000, 0x80260000, "nw4r::g3d + m3d"), (0x80260000, 0x80285800, "sonido: nw4r::snd"), (0x80285800, 0x802AB4F0, "nw4r::ef"),
    (0x802AB4F0, 0x802B8A70, "nw4r::lyt + m2d"), (0x802B8A70, 0x802DC000, "EGG"), (0x802DC000, 0x80300000, "MSL/rt"),
    (0x80760000, 0x81800000, "juego: REL (actores, escenas)"),
]


def lib_of(a):
    if a == 0x801B4EA0:
        return "bucle ocioso del SO"
    for lo, hi, n in SUBSIS:
        if lo <= a < hi:
            return n
    return "otro"


def dolphin_cycles(w):
    """Ciclos que el modelo de Dolphin (PPCTables.cpp) carga a cada instrucción (casi todas 1)."""
    w = w.astype(np.uint32)
    op = w >> 26
    x10 = (w >> 1) & 0x3FF
    x5 = (w >> 1) & 0x1F
    c = np.ones(len(w), dtype=np.int64)
    c[op == 7] = 3                                  # mulli
    c[(op == 46) | (op == 47)] = 11                 # lmw / stmw
    c[op == 17] = 2                                 # sc
    m31 = op == 31
    for x, v in ((491, 40), (459, 40), (75, 5), (11, 5), (235, 5), (598, 3), (982, 4), (86, 5), (54, 5), (278, 2),
                 (246, 2), (1014, 5), (758, 5), (470, 5), (467, 2), (597, 51), (533, 51), (661, 51), (725, 51),
                 (595, 3), (659, 3)):
        c[m31 & (x10 == x)] = v
    c[(op == 19) & (x10 == 50)] = 2                 # rfi
    c[(op == 59) & (x5 == 18)] = 17                 # fdivs
    c[(op == 63) & (x5 == 18)] = 31                 # fdiv
    c[(op == 4) & (x5 == 18)] = 17                  # ps_div
    c[(op == 4) & (x5 == 26)] = 2                   # ps_rsqrte
    m63 = op == 63
    for x in (70, 38, 134, 711):
        c[m63 & (x10 == x)] = 3                     # mtfsb0/1, mtfsfi, mtfsf
    return c


class Static:
    def __init__(self, tag="m2"):
        a = np.load(f"{V}/datos/a64/{tag}_static.npz")
        self.cls, self.flg, self.tgt, self.fnA = a["cls"], a["flg"], a["tgt"], a["fn"]
        self.funcsA = json.loads(str(a["funcs"]))
        self.clasesA = json.loads(str(a["clases"]))
        ln = np.load(f"{V}/datos/a64/{tag}_lines.npz")
        fil, lin = ln["fil"], ln["lin"]
        self.cab = json.loads(str(ln["cab"]))
        self.n = len(self.cls)
        self.src_funcs = json.load(open(f"{V}/datos/srcmap/funcs.json"))
        self.clasesS = json.load(open(f"{V}/datos/srcmap/clases.json"))
        self.base = np.zeros(88, dtype=np.int64)
        cls_l, fn_l = [], []
        P = {k: [] for k in ("gl", "ge", "addr", "word")}
        self.call_g, self.call_fn, self.disp_g = [], [], []
        tokg, tokid = [], []
        self.toks = json.load(open(f"{V}/datos/srcmap/toks.json"))
        self.nl = []
        for N in range(87):
            z = np.load(f"{V}/datos/srcmap/{N}.npz")
            n = len(z["cls"])
            self.nl.append(n)
            self.base[N + 1] = self.base[N] + n
            cls_l.append(z["cls"])
            fn_l.append(z["fn"])
            P["gl"].append(z["ppc_line"].astype(np.int64) + self.base[N])
            P["ge"].append(np.minimum(z["ppc_end"].astype(np.int64), n - 1) + self.base[N])
            P["addr"].append(z["ppc_addr"])
            P["word"].append(z["ppc_word"])
            self.call_g.append(z["call_line"].astype(np.int64) + self.base[N])
            self.call_fn.append(z["call_fn"])
            self.disp_g.append(z["disp_line"].astype(np.int64) + self.base[N])
            tokg.append(z["tok_line"].astype(np.int64) + self.base[N])
            tokid.append(z["tok_id"])
        self.tok_g, self.tok_id = np.concatenate(tokg), np.concatenate(tokid)
        self.G_cls = np.concatenate(cls_l)
        self.G_fn = np.concatenate(fn_l)
        self.nlines = len(self.G_cls)
        self.ppc_gl, self.ppc_ge = np.concatenate(P["gl"]), np.concatenate(P["ge"])
        self.ppc_addr, self.ppc_word = np.concatenate(P["addr"]), np.concatenate(P["word"])
        self.ppc_fn = self.G_fn[self.ppc_gl]            # función (del fuente) que contiene la instrucción
        self.ppc_cyc = dolphin_cycles(self.ppc_word)
        self.call_g, self.call_fn, self.disp_g = (np.concatenate(x) for x in (self.call_g, self.call_fn, self.disp_g))
        # instrucciones AArch64 de funciones generadas
        isgen = np.array([bool(GEN.search(f[0])) for f in self.funcsA])
        self.gen = (self.fnA >= 0) & isgen[np.maximum(self.fnA, 0)]
        self.gidx = np.where(fil >= 0, self.base[np.maximum(fil, 0)] + lin, -1)
        self.gidx[~self.gen] = -1
        self.hdr = (fil <= -10) & self.gen
        self.hid = np.where(fil <= -10, -(fil.astype(np.int64) + 10), -1)
        self.hlin = lin
        self.blk = np.cumsum(self.flg & 1, dtype=np.int64) - 1
        self.nblk = int(self.blk[-1]) + 1
        self.blk_first = np.flatnonzero(self.flg & 1)
        self.blk_last = np.r_[self.blk_first[1:] - 1, self.n - 1]
        nameA = {f[0]: i for i, f in enumerate(self.funcsA)}
        # función del fuente -> función AArch64 (mismo nombre)
        self.s2a = np.array([nameA.get(f[0], -1) for f in self.src_funcs], dtype=np.int64)
        # archivo output_N de cada función AArch64 generada (-1 si no es del juego)
        self.tuA = np.full(len(self.funcsA), -1, dtype=np.int64)
        for i, f in enumerate(self.src_funcs):
            if self.s2a[i] >= 0:
                self.tuA[self.s2a[i]] = f[1]
        # "instancias" de código de cabecera: tramos seguidos de instrucciones de la misma línea de cabecera
        # dentro de un bloque. clave = (archivo N, cabecera, línea)
        tu = self.tuA[np.maximum(self.fnA, 0)]
        key = np.where(self.hdr & (tu >= 0), (tu << 40) | (self.hid << 24) | lin.astype(np.int64), -1)
        start = key >= 0
        start[1:] &= (key[1:] != key[:-1]) | ((self.flg[1:] & 1) != 0)
        self.run_i = np.flatnonzero(start)
        self.run_key = key[self.run_i]
        self.run_blk = self.blk[self.run_i]


def load_seg(S, path):
    """-> (G: cuenta por línea global, -1 = línea sin instrumentar; funcs: nombre -> entradas)."""
    z = np.load(path)
    G = np.full(S.nlines, -1, dtype=np.int64)
    for N in range(87):
        if f"L_{N}" not in z:
            continue
        G[S.base[N] + z[f"Z_{N}"]] = 0
        G[S.base[N] + z[f"L_{N}"]] = z[f"C_{N}"]
    H = {}
    if "cab" in z:
        for N, d in json.loads(str(z["cab"])).items():
            for h, lines in d.items():
                if h in S.cab:
                    hid = S.cab.index(h)
                    for l, c in lines.items():
                        H[(int(N) << 40) | (hid << 24) | int(l)] = c
    return G, json.loads(str(z["funcs"])), H


def add_segs(S, paths):
    G, F, H = None, {}, {}
    for p in paths:
        g, f, h = load_seg(S, p)
        for k, v in h.items():
            H[k] = H.get(k, 0) + v
        if G is None:
            G = g
        else:
            both = (G >= 0) & (g >= 0)
            G = np.where(both, G + g, np.maximum(G, g))
        for k, v in f.items():
            F[k] = F.get(k, 0) + v
    return G, F, H


def ppc_counts(S, G):
    """Veces que se ejecutó cada instrucción PPC (exacto: la cuenta de su primera sentencia instrumentada)."""
    inst = np.flatnonzero(G >= 0)
    idx = np.searchsorted(inst, S.ppc_gl, side="right")
    ok = idx < len(inst)
    first = inst[np.minimum(idx, len(inst) - 1)]
    ok &= first < S.ppc_ge
    c = np.where(ok, G[first], -1)
    # sin sentencia propia (nop, sentencia eliminada por muerta): la cuenta de la instrucción anterior de su función
    last_ok = np.maximum.accumulate(np.where(ok, np.arange(len(c)), -1))
    same = (last_ok >= 0) & (S.ppc_fn[np.maximum(last_ok, 0)] == S.ppc_fn)
    c = np.where(ok, c, np.where(same, c[np.maximum(last_ok, 0)], 0))
    return c, ok


def a64_counts(S, G, H=None, diag=None):
    """-> (ex_cal, ex_lo, ex_hi, c, ex_flujo): ejecuciones de cada instrucción AArch64 generada (0 fuera).

    ex_cal: bloques con línea propia = mediana exacta; bloques solo de cabecera = cota alta × r, con r calibrado
    por línea de cabecera para que el total de ejecuciones de esa línea (por archivo) cuadre con el de gcov."""
    c = np.where(S.gidx >= 0, G[np.maximum(S.gidx, 0)], -1)
    v = c >= 0
    keys, vals = S.blk[v], c[v]
    o = np.lexsort((vals, keys))
    ks, vs = keys[o], vals[o]
    st = np.flatnonzero(np.r_[True, ks[1:] != ks[:-1]])
    cnt = np.diff(np.r_[st, len(ks)])
    bc = np.full(S.nblk, -1, dtype=np.int64)
    bc[ks[st]] = vs[st + cnt // 2]
    # bloques del código generado sin ninguna línea propia
    bgen = S.gen[S.blk_first]
    unk = np.flatnonzero((bc < 0) & bgen)
    lo, hi, med = bc.copy(), bc.copy(), bc.copy()
    lo[unk] = 0
    fnb = S.fnA[S.blk_first]
    last_flg = S.flg[S.blk_last]
    # predecesores por salto de cada bloque desconocido
    src = np.flatnonzero((S.tgt >= 0) & ((S.flg & (2 | 16)) != 0) & S.gen)
    tb = S.blk[np.minimum(S.tgt[src], S.n - 1)]
    isunk = np.zeros(S.nblk, dtype=bool)
    isunk[unk] = True
    m = isunk[tb] & (S.blk_first[tb] == S.tgt[src])
    preds = {}
    for s_, b_ in zip(src[m].tolist(), tb[m].tolist()):
        preds.setdefault(b_, []).append(s_)
    for _ in range(3):
        for b in unk.tolist():
            ft = b > 0 and fnb[b - 1] == fnb[b] and not (last_flg[b - 1] & 2)
            # cota alta: hereda del anterior si se entra cayendo; si solo por saltos, el mayor predecesor
            h = hi[b - 1] if ft else 0
            tot, known = 0, True
            if ft:
                p = b - 1
                if med[p] < 0:
                    known = False
                elif last_flg[p] & 16:                      # el anterior acaba en salto condicional
                    t = S.tgt[S.blk_last[p]]
                    tbk = S.blk[t] if 0 <= t < S.n else -1
                    if tbk >= 0 and med[tbk] >= 0 and tbk != b:
                        tot += max(0, med[p] - med[tbk])
                    else:
                        tot += med[p]
                else:
                    tot += med[p]
            for s_ in preds.get(b, ()):
                p = S.blk[s_]
                h = max(h, hi[p] if hi[p] >= 0 else 0)
                if med[p] < 0:
                    known = False
                    continue
                if S.flg[s_] & 16:                          # salto condicional: tomado = total - caída
                    nb = p + 1
                    if nb < S.nblk and med[nb] >= 0 and nb != b:
                        tot += max(0, med[p] - med[nb])
                    else:
                        tot += med[p]
                else:
                    tot += med[p]
            hi[b] = max(h, 0)
            if known or tot > 0:
                med[b] = tot
    med[(med < 0) & bgen] = 0
    hi[(hi < 0) & bgen] = 0
    cal = med.copy()
    if H:
        hk = np.array(sorted(H), dtype=np.int64)
        hv = np.array([H[k] for k in hk.tolist()], dtype=np.float64)
        uk, inv = np.unique(S.run_key, return_inverse=True)
        rb = S.run_blk
        known_run = ~isunk[rb]
        pk = np.bincount(inv[known_run], weights=bc[rb[known_run]].astype(np.float64), minlength=len(uk))
        pu = np.bincount(inv[~known_run], weights=hi[rb[~known_run]].astype(np.float64), minlength=len(uk))
        pos = np.searchsorted(hk, uk)
        has = (pos < len(hk)) & (hk[np.minimum(pos, len(hk) - 1)] == uk)
        T = np.where(has, hv[np.minimum(pos, len(hk) - 1)], -1.0)
        r = np.full(len(uk), np.nan)
        m = has & (pu > 0)
        r[m] = np.clip((T[m] - pk[m]) / pu[m], 0.0, 1.0)
        # r de cada bloque desconocido = el menor r de sus tramos de cabecera (los que tienen dato)
        rblk = np.full(S.nblk, np.inf)
        ur = ~known_run & ~np.isnan(r[inv])
        np.minimum.at(rblk, rb[ur], r[inv[ur]])
        okb = isunk & np.isfinite(rblk)
        cal[okb] = np.rint(hi[okb] * rblk[okb]).astype(np.int64)
        if diag is not None:
            nins = (S.blk_last - S.blk_first + 1).astype(np.float64)
            diag["peso_alto_bloques_sin_linea"] = float((hi[unk] * nins[unk]).sum())
            diag["peso_alto_calibrado"] = float((hi[okb] * nins[okb]).sum())
            diag["peso_cal"] = float((cal[unk] * nins[unk]).sum())
            # comprobación: líneas de cabecera que solo aparecen en bloques conocidos -> gcov frente a predicho
            chk = has & (pu == 0) & (T > 0)
            diag["cab_solo_conocidos_gcov"] = float(T[chk].sum())
            diag["cab_solo_conocidos_pred"] = float(pk[chk].sum())
    out = []
    for x in (cal, lo, hi, med):
        e = x[S.blk]
        e[~S.gen] = 0
        e[e < 0] = 0
        out.append(e)
    return out[0], out[1], out[2], c, out[3]


def dx_lines(run):
    """Líneas [DX] de runs/<run>/dx.txt -> lista de dicts."""
    rows = []
    for ln in open(os.path.join(V, "runs", run, "dx.txt")):
        if not ln.startswith("[DX] ") or " escena=" not in ln:
            continue
        p = ln.split()
        d = {"ev": p[1]}
        for kv in p[2:]:
            if "=" in kv:
                k, v = kv.split("=", 1)
                try:
                    d[k] = float(v) if "." in v else int(v)
                except ValueError:
                    d[k] = v
        rows.append(d)
    return rows


def segments(run):
    """-> lista de segmentos cerrados: dict(n, escena, seg, y deltas de todos los contadores)."""
    rows = dx_lines(run)
    ini, segs = {}, []
    for r in rows:
        if r["ev"] == "ini":
            ini[r["n"]] = r
        elif r["ev"] == "fin" and r["n"] in ini:
            a = ini[r["n"]]
            d = {"n": r["n"], "escena": r["escena"], "seg": r["seg"]}
            for k, v in r.items():
                if isinstance(v, (int, float)) and k not in ("n",) and k in a and isinstance(a[k], (int, float)):
                    d[k] = v - a[k]
            d["x86_run"] = r.get("x86_run", 0)
            segs.append(d)
    return segs
