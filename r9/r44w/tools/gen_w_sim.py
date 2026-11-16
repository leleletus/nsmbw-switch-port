#!/usr/bin/env python3
"""[r44w] gen_w_sim.py <export dir> <salida.inc> [--fpr] [--cnt]

Simulación de la superficie del agua (frente W, NWII_W_SIM). Copia del CÓDIGO GENERADO exacto de:
  - func_807B5240_g6 (output_48.cpp; execute del actor del REL, llega por g_disp_table) -> w_sim_807B5240_g6, con su
    llamada directa a func_800ECAE0_g6 cambiada por w_sim_800ECAE0_g6;
  - func_800ECAE0_g6 (output_13.cpp; el cálculo de las olas) -> w_sim_800ECAE0_g6.
--cnt: contadores en las cabezas de los bucles (estadística, W_SIM_CNT).
--fpr: los 32 FPR y sus ps1 en variables locales (WFn/WPn) en vez de ctx.fpr/ctx.ps1 (memoria); se vuelcan a ctx
       antes de toda llamada, sondeo lento y return, y se recargan después (W_FOUT/W_FIN). Mismas operaciones
       (mismas expresiones de cpu_context.h / g7.h), solo cambia dónde viven los valores.
"""
import re
import sys

EXP, OUT = sys.argv[1], sys.argv[2]
FPR = "--fpr" in sys.argv
CNT = "--cnt" in sys.argv
TRACE = "--trace" in sys.argv
INL = next((a.split("=", 1)[1].split(",") for a in sys.argv if a.startswith("--inline=")), [])
PFX = next((a.split("=", 1)[1] for a in sys.argv if a.startswith("--prefix=")), "w_sim")
LOOPS = ["800ED074", "800ED4AC", "800ED570", "800ED6C0", "800ED8C4", "800ED9B8", "800EDC60"]


def extract(path, name):
    lines = open(path).read().split("\n")
    s = next(i for i, l in enumerate(lines) if l.startswith('extern "C" uint64_t %s(' % name))
    e = next(i for i in range(s + 1, len(lines)) if lines[i] == "}")
    return lines[s:e + 1]


R = lambda n: r"(\d+)"


def fpr_line(l, st, inlambda=False):
    """reescribe una línea con FPR en locales; devuelve lista de líneas"""
    code, sep, com = l.partition(" // ")
    o = code
    inl_call = False
    for x in INL:
        if "func_%s_g6(ctx, " % x in o:
            o = o.replace("func_%s_g6(ctx, " % x, "w_inl_%s(" % x)
            inl_call = True
    # helpers de CPUContext (índices constantes)
    o = re.sub(r"ctx\.fill\((\d+), ", lambda m: "W_FILL(%s, " % m.group(1), o)
    o = re.sub(r"ctx\.(fadds|fsubs|fdivs)\((\d+), (\d+), (\d+)\);",
               lambda m: "W_%s(%s, %s, %s);" % (m.group(1).upper(), m.group(2), m.group(3), m.group(4)), o)
    o = re.sub(r"ctx\.frsp\((\d+), (\d+)\);", r"W_FRSP(\1, \2);", o)
    o = re.sub(r"ctx\.fctiwz\((\d+), (\d+)\);", r"W_FCTIWZ(\1, \2);", o)
    o = re.sub(r"g7::fmuls\(ctx, (\d+), (\d+), (\d+)\);", r"W_FMULS(\1, \2, \3);", o)
    o = re.sub(r"g7::psq_l<(\d+)u>\(ctx, mv_, (\d+), ", r"W_PSQ_L(\1, \2, ", o)
    o = re.sub(r"g7::psq_st<(\d+)u>\(ctx, mv_, (\d+), ", r"W_PSQ_ST(\1, \2, ", o)
    o = re.sub(r"ctx\.fpr\[(\d+)\]", r"WF\1", o)
    o = re.sub(r"ctx\.ps1\[(\d+)\]", r"WP\1", o)
    if re.search(r"ctx\.(f[a-z]+|ps_?[a-z]*|fpr|ps1|set_ps|set_bits|lfs|stfs)\b", o) or re.search(r"g7::(fm|fn|ps_|psq)", o):
        st["left"].append(l.strip()[:160])
    full = o + (sep + com if sep else "")
    # sincronización: llamadas, despacho, sondeo lento, returns
    pre, post = [], []
    if inl_call:
        # llamada a un llamado integrado (lambda con los mismos FPR locales): sin volcados. Su salida G6BIT solo
        # ocurre tras un sondeo que ya volcó (y puede haber cambiado de hilo): se devuelve sin volcar, como antes.
        st["inl"] = st.get("inl", 0) + 1
        return [full]
    if "process_pending_callbacks_slow(ctx)" in full:
        full = full.replace("process_pending_callbacks_slow(ctx)) return", "process_pending_callbacks_slow((W_FOUT(), ctx))) return")
        full = full.replace("G7CR_LOAD(0);", "G7CR_LOAD(0); W_FIN();", 1)
        st["polls"] += 1
    elif re.search(r"\w+_[0-9A-F]{8}\w*_g6\(ctx|nwii_dispatch\(ctx|g7_bctrl_g6\(|resume_pruned\(ctx", full):
        pre.append("    W_FOUT();")
        mg = re.search(r"(goto \w+;)\s*$", full)
        if mg:  # la línea acaba saltando: recargar antes del goto (detrás no se llegaría)
            full = full[:mg.start()] + "W_FIN(); " + mg.group(1)
            st["calls_goto"] = st.get("calls_goto", 0) + 1
        else:
            post.append("    W_FIN();")
        st["calls"] += 1
    elif re.search(r"\breturn\b", code) and not inlambda:
        # `if (...) { ...; return ...; }` en una línea: el volcado DENTRO del bloque (camino frío); si no, antes
        if re.search(r"\{[^{}]*\breturn\b[^{}]*\}", full):
            full = re.sub(r"\breturn\b", "W_FOUT(); return", full, count=1)
        else:
            pre.append("    W_FOUT();")
        st["rets"] += 1
    return pre + [full] + post


def main():
    t48 = extract(EXP + "/output_48.cpp", "func_807B5240_g6")
    f13 = extract(EXP + "/output_13.cpp", "func_800ECAE0_g6")
    st = {"left": [], "polls": 0, "calls": 0, "rets": 0}
    lambdas = []
    import glob
    for x in INL:
        src = next(f for f in sorted(glob.glob(EXP + "/output_*.cpp")) if ('uint64_t func_%s_g6(nwii' % x) in open(f).read())
        fb = extract(src, "func_%s_g6" % x)
        lambdas.append("    auto w_inl_%s = [&](uint64_t G5ic_in, uint64_t G6lr0) __attribute__((always_inline)) -> uint64_t {  // [r44w] %s integrado" % (x, x))
        stl = {"left": [], "polls": 0, "calls": 0, "rets": 0}
        for j, l in enumerate(fb[1:-1]):
            if l.startswith("loc_") or l.lstrip().startswith("//"):
                lambdas.append(l.replace("loc_", "loc_i%s_" % x) if l.startswith("loc_") else l)
                continue
            ll = fpr_line(l, stl, inlambda=True)
            ll = [re.sub(r"\bloc_", "loc_i%s_" % x, q) for q in ll]
            lambdas += ll
        if stl["left"] or stl["calls"] > 1:
            print("gen_w_sim: el llamado %s no se puede integrar: %s" % (x, stl)); sys.exit(4)
        lambdas.append("    };")
        st["polls_inl"] = st.get("polls_inl", 0) + stl["polls"]
    body = []
    for i, l in enumerate(f13):
        if i == 1 and "MemView mv_" in l and FPR:
            body.append(l)
            body.append("    W_FDECL; W_FIN();  // [r44w] FPR en locales")
            body += lambdas
            continue
        m = re.match(r"^loc_([0-9A-F]{8}): ;$", l)
        if m and CNT and m.group(1) in LOOPS:
            body.append(l)
            body.append("    W_SIM_CNT(%d);" % LOOPS.index(m.group(1)))
            continue
        mt = re.match(r"^\s*// 0x([0-9A-F]{8}): ", l)
        if TRACE and mt:
            body.append(l)
            body.append("    W_TR(0x%su);" % mt.group(1))
            continue
        if FPR and i > 1 and not l.startswith("loc_") and not l.lstrip().startswith("//"):
            body += fpr_line(l, st)
        else:
            body.append(l)
    body = [l.replace("func_800ECAE0_g6", PFX + "_800ECAE0_g6") for l in body]
    t48 = [l.replace("func_800ECAE0_g6", PFX + "_800ECAE0_g6").replace("func_807B5240_g6", PFX + "_807B5240_g6") for l in t48]
    o = ["// [r44w] GENERADO por r9/r44w/tools/gen_w_sim.py (%s%s) a partir de %s (NO editar a mano)" % (
        "fpr " if FPR else "", "cnt" if CNT else "", EXP),
         '#include "w_sim_fpr.h"',
         'extern "C" uint64_t %s_800ECAE0_g6(nwii::runtime::CPUContext& ctx, uint64_t G5ic_in, uint64_t G6lr0);' % PFX, ""]
    o += body + [""] + t48 + [""]
    o += ['void %s_807B5240(CPUContext& ctx) { %s_807B5240_g6(ctx, ctx.inst_count, (uint64_t)ctx.lr | G6W | G6CHK); }' % (PFX, PFX), ""]
    open(OUT, "w").write("\n".join(o))
    print("gen_w_sim: fpr=%s cnt=%s polls=%d calls=%d (con goto %d) rets=%d integradas=%d (sondeos en ellas %d) sin_traducir=%d" % (FPR, CNT, st["polls"], st["calls"], st.get("calls_goto", 0), st["rets"], st.get("inl", 0), st.get("polls_inl", 0), len(st["left"])))
    for l in st["left"][:20]:
        print("   QUEDA: " + l)
    if st["left"]:
        sys.exit(3)


if __name__ == "__main__":
    main()
