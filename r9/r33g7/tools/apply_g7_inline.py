#!/usr/bin/env python3
"""apply_g7_inline.py: segunda ronda de integracion de llamados (inline), guiada por las cuentas EXACTAS de DX.

  apply_g7_inline.py plan  [--max-ppc 200] [--min-calls 2]      -> diag/inl_sel.tsv (+ resumen)
  apply_g7_inline.py apply <export_pre> --sites diag/inl_sel.tsv [--stats]

Trabaja sobre el texto de ANTES de G5 (copia de r9/r20g6/e2/export_pre: ya lleva la primera ronda, apply_g6_inline.py
de G6 e2, que solo integro llamados de <= 32 PPC y, por un fallo de su prueba de "declaracion", ninguno que tuviera
psq_l/psq_st: PSMTXCopy, PSMTXConcat, nw4r::math...). Mismas reglas y misma justificacion de exactitud que
apply_g6_inline.py (ver su cabecera: el cuerpo del llamado se copia en el sitio; cada `blr` pasa a
`ctx.pc = <lr>; goto <continuacion>;` y la continuacion es la comprobacion del propio llamador), con tres cambios:
  - el sitio se elige por datos: (funcion, direccion del `bl`, n.o de aparicion de esa direccion en la funcion)
    con sus llamadas por fotograma medidas por DX (gcov, r32dx: e2 tiene las mismas instrucciones PPC en el mismo
    orden que export_pre);
  - tambien sitios que estan dentro de una copia de la primera ronda (su comprobacion es
    `if (ctx.pc != 0xRET) goto loc_2...;` en vez de `... return;`) y llamados que ya llevan copias dentro
    (se renombran TODAS las etiquetas del cuerpo con un prefijo unico);
  - una linea que empieza por `nwii::runtime::` o `std::` es una sentencia, no una declaracion.
"""
import collections
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import g6_inline_orig as G  # noqa: E402  (apply_g6_inline.py de r20g6, sin cambios)

R = "/home/djvemo/nsmbw-port/r9"
PRE0 = R + "/r20g6/e2/export_pre"
MARK = "// [r33-g7i]"
PPCM = re.compile(r"^\s*// 0x([0-9A-F]{8}): [0-9A-F]{8}")
CHECKG = re.compile(r"^(\s+)if \(ctx\.pc != 0x([0-9A-F]+)\) goto (loc_[0-9A-F]+);$")
DECL = re.compile(r"^    (const |static )?(u?int\d+_t|double|float|bool|auto|char|unsigned|int|long)\b")
LABEL_ANY = re.compile(r"\bloc_([0-9A-F]+)\b")


def eligible(name, body, max_ppc):
    """apply_g6_inline.eligible con la prueba de declaracion corregida y las formas de la primera ronda."""
    if body is None:
        return "shape"
    ppc = sum(1 for l in body if l.startswith("    // 0x"))
    if ppc > max_ppc:
        return "size"
    labels = {m.group(1) for l in body for m in [G.LABEL.match(l)] if m}
    if not body or not G.LABEL.match(body[0]):
        return "no entry label"
    m = re.search(r"([0-9A-F]{8})$", name)
    if not m or G.LABEL.match(body[0]).group(1) != m.group(1):
        return "entry not first"
    esc = False
    for l in body:
        code = l.split("//")[0] if "//" in l and not l.lstrip().startswith("//") else ("" if l.lstrip().startswith("//") else l)
        s = code.strip()
        if not s:
            continue
        if DECL.match(code):
            return "declaration"
        if "nwii_entry" in code or "temp_lr" in code:
            return "indirect/tail"
        if ("nwii_dispatch" in code or "native_stack_ok" in code) and not (G.TAIL.match(l) or G.BCTRL.match(l) or TAILG.match(l)):
            return "indirect/tail"
        if "ctx.lr =" in code or "ctx.lr=" in code:
            esc = True
        for g in G.GOTO.findall(code):
            if g not in labels:
                return "goto outside"
        if "return" in code:
            if not (l == G.BLR or G.CBLR.match(l) or G.POLL.match(l) or G.PCCHECK.match(l) or G.TAIL.match(l) or G.BCTRL.match(l)):
                return "return form"
        if "switch" in code or "case " in code:
            return "switch"
        if G.ESC.search(code.replace("psq_l_fast", "X").replace("psq_st_fast", "X")) or "_io(" in code:
            esc = True
    return ("ok", ppc, esc)


# llamada de cola de un llamado ya integrado en la primera ronda: `...; T(ctx); goto loc_2...;`
TAILG = re.compile(r"^(\s+)(ctx\.pc = 0x[0-9A-F]+; if \(nwii::runtime::native_stack_ok\(\)\) \w+\(ctx\);|ctx\.pc = ctx\.ctr; nwii_dispatch\(ctx\);) goto loc_[0-9A-F]+;$")


def read_funcs(d):
    files = sorted((x for x in os.listdir(d) if re.match(r"output_\d+\.cpp$", x)), key=lambda x: int(x[7:-4]))
    srcs, funcs = {}, {}
    for x in files:
        src = open(os.path.join(d, x)).read().split("\n")
        srcs[x] = src
        i = 0
        while i < len(src):
            m = G.FUNC.match(src[i])
            if m:
                j = i + 1
                while src[j] != "}":
                    j += 1
                funcs[m.group(1)] = src[i:j + 1]
                i = j + 1
            else:
                i += 1
    return files, srcs, funcs


def plan(argv):
    import numpy as np
    max_ppc = int(argv[argv.index("--max-ppc") + 1]) if "--max-ppc" in argv else 200
    min_calls = float(argv[argv.index("--min-calls") + 1]) if "--min-calls" in argv else 2.0
    os.environ["DXV"] = R + "/r32dx"
    os.environ["DXTAG"] = "m2"
    sys.path.insert(0, R + "/r33g7/dx/tools")
    import dxlib
    S = dxlib.Static()
    esc = ["desierto4j", "agua_pinguino", "cielo_paratroopas", "nubes_plataformas", "castillo"]
    cnt = np.zeros(len(S.call_g))
    for e in esc:
        Gc, F, H = dxlib.add_segs(S, glob.glob(f"{dxlib.V}/datos/gcov60/*_{e}_main.npz"))
        fr = sum(s["xfb"] for s in dxlib.segments("gcov60") if s["seg"] == "main" and s["escena"] == e)
        cnt += np.maximum(Gc[S.call_g], 0) / fr / len(esc)
    names = [f[0] for f in S.src_funcs]
    order = np.argsort(S.ppc_gl, kind="stable")
    pg = S.ppc_gl[order]
    # n.o de aparicion de cada direccion dentro de su funcion (en orden de linea)
    occ = np.zeros(len(pg), dtype=np.int64)
    seen, last_fn = {}, None
    for j, i in enumerate(order.tolist()):
        f = int(S.ppc_fn[i])
        if f != last_fn:
            seen, last_fn = {}, f
        a = int(S.ppc_addr[i])
        occ[j] = seen.get(a, 0)
        seen[a] = occ[j] + 1
    sites = {}
    for g, f, v in zip(S.call_g.tolist(), S.call_fn.tolist(), cnt.tolist()):
        if v <= 0 or f < 0:
            continue
        j = int(np.searchsorted(pg, g, side="right")) - 1
        i = int(order[j])
        sites[(names[int(S.ppc_fn[i])], int(S.ppc_addr[i]), int(occ[j]))] = (names[f], v)
    files, srcs, funcs = read_funcs(PRE0)
    elig = {n: eligible(n, G.callee_body(fn), 10 ** 9) for n, fn in funcs.items()}
    tot = sum(v for _, v in sites.values())
    why = collections.Counter()
    rows = []
    for (fn, addr, k), (callee, v) in sites.items():
        e = elig.get(callee, "missing")
        if isinstance(e, tuple):
            rows.append((v, fn, addr, k, callee, e[1]))
        else:
            why[e] += v
    rows.sort(reverse=True)
    print(f"llamadas directas/fot (media de 5 escenas, e2): {tot:,.0f} en {len(sites)} sitios; a llamados integrables: "
          f"{sum(r[0] for r in rows):,.0f}; no integrables: " + ", ".join(f"{k} {v:,.0f}" for k, v in why.most_common()))
    for mp in (64, 128, 200, 400):
        for mc in (20, 5, 2, 1):
            sel = [r for r in rows if r[5] <= mp and r[0] >= mc]
            print(f"  PPC<={mp:4d} y >= {mc:2d} llamadas/fot: {len(sel):5d} sitios, {sum(r[0] for r in sel):8,.0f} llamadas/fot "
                  f"({100 * sum(r[0] for r in sel) / tot:4.1f} %), +{sum(r[5] for r in sel):7,d} PPC copiadas")
    sel = [r for r in rows if r[5] <= max_ppc and r[0] >= min_calls]
    with open(R + "/r33g7/diag/inl_sel.tsv", "w") as f:
        f.write(f"# funcion\tdireccion del bl\taparicion\tllamado\tPPC del llamado\tllamadas/fot   (max-ppc {max_ppc}, min-calls {min_calls})\n")
        for v, fn, addr, k, callee, ppc in sel:
            f.write(f"{fn}\t{addr:08X}\t{k}\t{callee}\t{ppc}\t{v:.3f}\n")
    print(f"elegidos: {len(sel)} sitios, {sum(r[0] for r in sel):,.0f} llamadas/fot ({100 * sum(r[0] for r in sel) / tot:.1f} %), "
          f"+{sum(r[5] for r in sel):,d} PPC -> diag/inl_sel.tsv")


def apply(argv):
    d = argv[0]
    sites_p = argv[argv.index("--sites") + 1]
    stats_only = "--stats" in argv
    want = collections.defaultdict(dict)
    for l in open(sites_p):
        if l.startswith("#") or not l.strip():
            continue
        p = l.rstrip("\n").split("\t")
        want[p[0]][(p[1], int(p[2]))] = p[3]
    files, srcs, funcs = read_funcs(d)
    for x in files:
        if srcs[x][0].startswith(MARK):
            sys.exit(f"{x}: ya integrado (ronda G7)")
    elig = {}
    for callees in want.values():
        for c in set(callees.values()):
            if c not in elig and c in funcs:
                e = eligible(c, G.callee_body(funcs[c]), 10 ** 9)
                if isinstance(e, tuple):
                    elig[c] = (G.callee_body(funcs[c]), e[1], e[2], any("mv_." in l or "mv_," in l for l in funcs[c]))
    st = dict(sites=0, ppc=0, callers=0, missed=0)
    uid = [0]
    out_files = {}
    missed = []
    for x in files:
        src = srcs[x]
        res = [MARK + " second inlining round, call sites chosen by exact call counts (r9/r33g7/tools/apply_g7_inline.py)"]
        i = 0
        while i < len(src):
            m = G.FUNC.match(src[i])
            if not m:
                res.append(src[i]); i += 1; continue
            j = i + 1
            while src[j] != "}":
                j += 1
            fn = src[i:j + 1]
            i = j + 1
            caller = m.group(1)
            w = want.get(caller)
            if not w:
                res.extend(fn); continue
            out = [fn[0]]
            need_mv = False
            has_mv = any(l.startswith("    nwii::runtime::MemView mv_(ctx.mmu);") for l in fn[1:3])
            seen, cur, done = {}, None, set()
            k = 1
            while k < len(fn) - 1:
                l = fn[k]
                pm = PPCM.match(l)
                if pm:
                    a = pm.group(1)
                    cur = (a, seen.get(a, 0))
                    seen[a] = cur[1] + 1
                cm = G.CALL.match(l)
                if cm and cur in w and w[cur] == cm.group(2) and cm.group(2) in elig and cm.group(2) != caller and k + 1 < len(fn):
                    chk = G.CHECK.match(fn[k + 1])
                    ret = chk.group(1) if chk else (CHECKG.match(fn[k + 1]).group(2) if CHECKG.match(fn[k + 1]) else None)
                    lr = G.LRSET.match(fn[k - 1])
                    if ret and lr and lr.group(1) == ret and int(cm.group(1), 16) == int(re.search(r"([0-9A-F]{8})$", cm.group(2)).group(1), 16):
                        callee = cm.group(2)
                        body, ppc, esc, uses_mv = elig[callee]
                        uid[0] += 1
                        tag = "3%08X" % uid[0]
                        labels = {mm.group(1) for bl in body for mm in [G.LABEL.match(bl)] if mm}
                        ren = lambda s: LABEL_ANY.sub(lambda mm: ("loc_" + tag + mm.group(1)) if mm.group(1) in labels else mm.group(0), s)
                        cont = "loc_4%08X%08X" % (uid[0], int(ret, 16))
                        lr_expr = "ctx.lr" if esc else "0x%s" % ret
                        out.append("    ctx.pc = 0x%s; // [g6i] %s inlined (%d PPC) [r33-g7i]" % (cm.group(1), callee, ppc))
                        for bl in body:
                            if bl == G.BLR:
                                out.append("    ctx.pc = %s; goto %s;" % (lr_expr, cont)); continue
                            c = G.CBLR.match(bl)
                            if c:
                                out.append("%sif (%s) { ctx.pc = %s; goto %s; }" % (c.group(1), ren(c.group(2)), lr_expr, cont)); continue
                            c = G.TAIL.match(bl)
                            if c:
                                out.append("%s%s goto %s;" % (c.group(1), c.group(2), cont)); continue
                            c = G.PCCHECK.match(bl)
                            if c:
                                out.append("%sif (ctx.pc != 0x%s) goto %s;" % (c.group(1), c.group(2), cont)); continue
                            out.append(ren(bl))
                        out.append(cont + ": ;")
                        out.append(fn[k + 1])
                        need_mv |= uses_mv
                        st["sites"] += 1; st["ppc"] += ppc
                        done.add(cur)
                        k += 2
                        continue
                out.append(l)
                k += 1
            out.append(fn[-1])
            if need_mv and not has_mv:
                out.insert(1, "    nwii::runtime::MemView mv_(ctx.mmu);")
            if done:
                st["callers"] += 1
            for key in w:
                if key not in done:
                    st["missed"] += 1; missed.append((caller,) + key + (w[key],))
            res.extend(out)
        out_files[x] = res
    print("g7i:", st)
    for mrow in missed[:12]:
        print("  no integrado:", mrow, "(llamado no integrable)" if mrow[3] not in elig else "(forma del sitio)")
    if stats_only:
        return
    for x, res in out_files.items():
        p = os.path.join(d, x)
        open(p + ".tmp", "w").write("\n".join(res))
        os.replace(p + ".tmp", p)
    print("g7i: escritos %d archivos" % len(out_files))


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "plan":
        plan(sys.argv[2:])
    elif len(sys.argv) > 2 and sys.argv[1] == "apply":
        apply(sys.argv[2:])
    else:
        sys.exit(__doc__)
