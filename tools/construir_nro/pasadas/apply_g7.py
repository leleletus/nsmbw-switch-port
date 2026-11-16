#!/usr/bin/env python3
"""apply_g7.py: pasada G7 sobre un export G6c3 (r9/r30g6c3/c3/export) -> codigo caliente mas compacto.

  apply_g7.py <export_dir> [--feat stfs,psq,fp,io,poll,...] [--jobs N]
  apply_g7.py --file in.inc out.inc [--feat ...]          (pruebas de CPU)

Reescribe en el sitio cada output_*.cpp (deben ser los de c3, sin pasar antes por G7), copia tools/g7.h al
export y anade `#include "g7.h"`. Cada transformacion ("feat") se puede quitar para aislar su efecto:

  stfs  ppc_to_single(...)                 -> g7::to_single(...)       (fcvt + comprobacion; denormales fuera de linea)
  psq   psq_l_fast / psq_st_fast / _io     -> g7::psq_l / psq_st / _io (camino general compartido fuera de linea)
  fp    ctx.fmuls / fmadds.. / ps_mul..    -> g7::...(ctx, ...)        (Force25Bit rapido, empate de fma fuera de linea)
  io    mv_.write{8,16,32,_f32}_io(...)    -> g7::write*_io(mv_, ...)  (tuberia de GX sin pasar por MMU::*_slow)
  poll  g6_poll_due(ctx)                   -> g7::poll_due(ctx)        (solo la cuenta atras; NECESITA el runtime de
                                                                         r9/r33g7/nWiiRuntime: ver LEEME)
  disp  bctrl por nwii_dispatch            -> g7_bctrl_g6(...)         (ayudante compartido con protocolo directo)
  cr    G5cNlt/gt/eq/so (4 bool por campo) -> campo CR perezoso y empaquetado (G7CR_*, G7CRB)
  icm   G5ic += N;                         -> G7IC(N);                 (macro: identico, salvo compilando con -DG7_NOIC)
Todas dan exactamente el mismo resultado que el original (r9/r33g7/LEEME.md).
"""
import argparse
import os
import re
import shutil
import sys
from concurrent.futures import ProcessPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ALL = ["stfs", "psq", "fp", "io"]

FP_METHODS = ["fmuls", "fmadds", "fmsubs", "fnmadds", "fnmsubs", "ps_mul", "ps_muls0", "ps_muls1", "ps_madd",
              "ps_msub", "ps_nmadd", "ps_nmsub", "ps_madds0", "ps_madds1"]


def rules(feats):
    R = []
    if "stfs" in feats:
        R.append((re.compile(r"\bnwii::runtime::ppc_to_single\("), "g7::to_single("))
    if "psq" in feats:
        R.append((re.compile(r"\bnwii::runtime::psq_l_fast<"), "g7::psq_l<"))
        R.append((re.compile(r"\bnwii::runtime::psq_st_fast<"), "g7::psq_st<"))
        R.append((re.compile(r"\bnwii::runtime::psq_st_io\("), "g7::psq_st_io("))
        R.append((re.compile(r"\bnwii::runtime::psq_l_io\("), "g7::psq_l_io("))
    if "fp" in feats:
        R.append((re.compile(r"\bctx\.(%s)\(" % "|".join(FP_METHODS)), r"g7::\1(ctx, "))
    if "io" in feats:
        R.append((re.compile(r"\bmv_\.(write8_io|write16_io|write32_io|write_f32_io)\("), r"g7::\1(mv_, "))
    if "poll" in feats:
        R.append((re.compile(r"\bg6_poll_due\(ctx\)"), "g7::poll_due(ctx)"))
    if "icm" in feats:
        # la cuenta de instrucciones por bloque pasa por una macro (G7IC): igual que antes salvo con -DG7_NOIC
        # (solo Switch, reloj real: inst_count no se usa para nada alli); G7IC_SLOW() va en el camino lento del sondeo.
        R.append((re.compile(r"\bG5ic \+= (\d+);"), r"G7IC(\1);"))
        R.append((re.compile(r"if \(NWII_UNLIKELY\((g6_poll_due|g7::poll_due)\(ctx\)\)\) \{ "), r"if (NWII_UNLIKELY(\1(ctx))) { G7IC_SLOW(); "))
    return R


MARK = "// [r33-g7]"

# ---- feat "disp": llamadas indirectas (bctrl) por un ayudante compartido con protocolo directo -------------
# c3:   G5lr = 0xR; // save return address
#       ctx.lr = G5lr; ctx.pc = G5pc; <volcados> ctx.inst_count = G5ic; ctx.ctr = G5ctr; [..]
#       if (ctx.ctr) { ctx.pc = ctx.ctr; nwii_dispatch(ctx); if (ctx.pc != 0xR) return g6_exact(ctx, G6lr0); }
#       else nwii::runtime::note_null_call(0xS, ctx.lr); <recargas>
# g7:   <volcados (sin lr/pc/inst_count)> ctx.ctr = G5ctr; [..]
#       { uint64_t t_ = g7_bctrl_g6(ctx, G5ic, 0xRu, G5pc); if (NWII_UNLIKELY((int64_t)t_ < 0)) return g6_exact(ctx, G6lr0); } <recargas>
# g7_bctrl_g6 (g7.h): si ctx.ctr es la entrada de una funcion _g6 (la misma a la que llegaria nwii_dispatch) la llama
# como una llamada directa (ic y lr en registros, sin envoltorio, sin `switch` de entrada, sin g6_slowret) y deja
# ctx.inst_count/pc/lr como los dejaba g6_slowret; si no, hace exactamente la sentencia de c3.
# Solo se transforma si la sentencia anterior es `G5lr = 0xR;` (LR = R) y S == R - 4; el resto se deja como esta.
RE_DISP = re.compile(
    r"^(\s*)ctx\.lr = G5lr; ctx\.pc = G5pc; ((?:ctx\.[^;]*; )*?)ctx\.inst_count = G5ic; ((?:ctx\.[^;]*; )*)"
    r"if \(ctx\.ctr\) \{ ctx\.pc = ctx\.ctr; nwii_dispatch\(ctx\); if \(ctx\.pc != 0x([0-9A-F]+)\) return g6_exact\(ctx, G6lr0\); \} "
    r"else nwii::runtime::note_null_call\(0x([0-9A-F]+), ctx\.lr\);(.*)$")
RE_SETLR = re.compile(r"^\s*G5lr = 0x([0-9A-F]+); // save return address\s*$")


# ---- feat "cr": campos de CR perezosos y empaquetados ------------------------------------------------------
# c3 guarda cada campo CR en 4 bool (G5cNlt/gt/eq/so): cada comparacion calcula los 4 (aunque solo se use uno en el
# salto y los demas solo en los volcados frios), cada volcado son 4 strb y cada recarga 4 ldrb.
# g7: por campo N con sus 4 bits declarados en la funcion, 4 locales (macros de g7.h):
#   G5cNm = 0: "modo comparacion": lt/gt/eq = G5cNa </>/== G5cNb (int64, operandos extendidos con o sin signo),
#              so = bit 31 de G5cNx (copia de XER en el momento de la comparacion);
#   G5cNm = 1: "modo palabra":     G5cNa = los 4 bytes de ctx.cr[N] tal cual (lt | gt << 8 | eq << 16 | so << 24).
# Comparaciones enteras -> G7CR_CMPS/CMPU (no calculan nada: guardan operandos); volcado de los 4 bits -> una
# palabra (G7CR_STORE); recarga -> una palabra (G7CR_LOAD); cualquier otra escritura de un bit -> G7CR_SET /
# G7CR_SET4 (modo palabra); cualquier lectura -> G7CRB(N, bit). El valor de cada bit en cada punto es el mismo.
CRBITS = ("lt", "gt", "eq", "so")
RE_G6FN = re.compile(r'^extern "C" uint64_t \w+_g6\(')
RE_CRTOK = re.compile(r"\bG5c([0-7])(lt|gt|eq|so)\b")
RE_CRDEF_LINE = re.compile(r"^(\s*)G5c([0-7])(lt|gt|eq|so) = (.*);(\s*//.*)?$")
OPND = r"(G5r\d+|ctx\.gpr\[\d+\]|-?\d+)"
RE_LT_S = re.compile(r"^\(\(int32_t\)%s < (?:\(int32_t\))?%s\)$" % (OPND, OPND))
RE_LT_U = re.compile(r"^\(%s < %s\)$" % (OPND, OPND))
RE_SO = re.compile(r"^\((G5xer|ctx\.xer) >> 31\) & 1$")


def cr_fn(lines, n):
    """lines: las lineas de una funcion _g6 (de la cabecera a la llave final). Devuelve las nuevas."""
    di = next((i for i, l in enumerate(lines[:6]) if "// [r14-g5] [r30-g6c3]" in l), None)
    if di is None:
        return lines
    full = [N for N in range(8) if all("bool G5c%d%s = ctx.cr[%d].%s;" % (N, b, N, b) in lines[di] for b in CRBITS)]
    if not full:
        return lines
    d = lines[di]
    for N in full:
        d = d.replace("bool G5c%deq = ctx.cr[%d].eq;" % (N, N), "G7CR_DECL(%d);" % N)
        for b in ("gt", "lt", "so"):
            d = d.replace(" bool G5c%d%s = ctx.cr[%d].%s;" % (N, b, N, b), "")
        assert "bool G5c%d" % N not in d, d
    fs = set(full)
    out = lines[:di] + [d]
    i = di + 1
    L = len(lines)

    def defline(j, N, bit):
        if j >= L:
            return None
        m = RE_CRDEF_LINE.match(lines[j])
        if m and int(m.group(2)) == N and m.group(3) == bit:
            return m
        return None

    def uses(code):
        return RE_CRTOK.sub(lambda m: "G7CRB(%s, %s)" % (m.group(1), m.group(2)) if int(m.group(1)) in fs else m.group(0), code)

    while i < L:
        line = lines[i]
        if "G5c" not in line:
            out.append(line); i += 1; continue
        m0 = RE_CRDEF_LINE.match(line)
        if m0 and int(m0.group(2)) in fs and m0.group(3) == "lt":
            N = int(m0.group(2))
            m1, m2, m3 = defline(i + 1, N, "gt"), defline(i + 2, N, "eq"), defline(i + 3, N, "so")
            if m1 and m2 and m3:
                ind, cm = m0.group(1), (m0.group(5) or m1.group(5) or m2.group(5) or m3.group(5) or "")
                e0, e1, e2, e3 = m0.group(4), m1.group(4), m2.group(4), m3.group(4)
                done = False
                mso = RE_SO.match(e3)
                for rx, mac in ((RE_LT_S, "G7CR_CMPS"), (RE_LT_U, "G7CR_CMPU")):
                    ml = rx.match(e0)
                    if not ml or not mso:
                        continue
                    X, Y = ml.group(1), ml.group(2)
                    if mac == "G7CR_CMPS":
                        yc = Y if re.fullmatch(r"-?\d+", Y) else "(int32_t)" + Y
                        gts = ["((int32_t)%s > %s)" % (X, yc)]
                        eqs = ["((int32_t)%s == %s)" % (X, yc)] + (["(%s == %s)" % (X, Y)] if re.fullmatch(r"-?\d+", Y) else [])
                    else:
                        gts = ["(%s > %s)" % (X, Y)]
                        eqs = ["(%s == %s)" % (X, Y)]
                    if e1 in gts and e2 in eqs:
                        out.append("%s%s(%d, %s, %s, %s);%s" % (ind, mac, N, X, Y, mso.group(1), cm))
                        n[mac] = n.get(mac, 0) + 1
                        done = True
                        break
                if not done and not any(("G5c%d" % N) in e for e in (e0, e1, e2, e3)):
                    out.append("%sG7CR_SET4(%d, %s, %s, %s, %s);%s" % (ind, N, uses(e0), uses(e1), uses(e2), uses(e3), cm))
                    n["G7CR_SET4 (lineas)"] = n.get("G7CR_SET4 (lineas)", 0) + 1
                    done = True
                if done:
                    i += 4; continue
            if m1 and m2 and not m3:
                # forma de registro en tres lineas (lt, gt, eq; so no se toca)
                ml = RE_LT_S.match(m0.group(4))
                if ml and re.fullmatch(r"-?\d+", ml.group(2)) and m1.group(4) == "((int32_t)%s > %s)" % ml.groups() \
                        and m2.group(4) in ("((int32_t)%s == %s)" % ml.groups(), "(%s == %s)" % ml.groups()):
                    out.append("%sG7CR_CMPS3(%d, %s, %s);%s" % (m0.group(1), N, ml.group(1), ml.group(2),
                                                                 m0.group(5) or m1.group(5) or m2.group(5) or ""))
                    n["G7CR_CMPS3"] = n.get("G7CR_CMPS3", 0) + 1
                    i += 3; continue
        # dentro de la linea
        ci = line.find(" //")
        code, cm = (line[:ci], line[ci:]) if ci >= 0 else (line, "")
        for N in full:
            pre = "G5c%d" % N
            if pre not in code:
                continue
            dump = "ctx.cr[%d].eq = G5c%deq; ctx.cr[%d].gt = G5c%dgt; ctx.cr[%d].lt = G5c%dlt; ctx.cr[%d].so = G5c%dso;" % ((N,) * 8)
            k = code.count(dump)
            if k:
                code = code.replace(dump, "G7CR_STORE(%d);" % N); n["G7CR_STORE"] = n.get("G7CR_STORE", 0) + k
            code, k = re.subn(r"(?:G5c%d(eq|gt|lt|so) = ctx\.cr\[%d\]\.\1; ?)+" % (N, N),
                              lambda m: "G7CR_LOAD(%d);" % N + (" " if m.group(0).endswith(" ") else ""), code)
            n["G7CR_LOAD"] = n.get("G7CR_LOAD", 0) + k
            code, k = re.subn(r"G5c%dlt = ([^;]*); G5c%dgt = ([^;]*); G5c%deq = ([^;]*); G5c%dso = ([^;]*);" % (N, N, N, N),
                              lambda m: m.group(0) if pre in "".join(m.groups()) else "G7CR_SET4(%d, %s, %s, %s, %s);" % ((N,) + m.groups()), code)
            # (la cuenta incluye los que se dejan; los que se dejan caen en G7CR_SET abajo)
            # forma de registro (Rc=1 de extsb., and., ...): escribe lt/gt/eq y NO so (so se queda como estaba)
            m3 = re.match(r"^(\s*)G5c%dlt = ([^;]*); G5c%dgt = ([^;]*); G5c%deq = ([^;]*);$" % (N, N, N), code)
            if m3:
                ml = RE_LT_S.match(m3.group(2))
                if ml and re.fullmatch(r"-?\d+", ml.group(2)) and m3.group(3) == "((int32_t)%s > %s)" % ml.groups() \
                        and m3.group(4) in ("((int32_t)%s == %s)" % ml.groups(), "(%s == %s)" % ml.groups()):
                    code = "%sG7CR_CMPS3(%d, %s, %s);" % (m3.group(1), N, ml.group(1), ml.group(2))
                    n["G7CR_CMPS3"] = n.get("G7CR_CMPS3", 0) + 1
            code, k = re.subn(r"(^\s*|(?<=; )|(?<=\{ )|(?<=\) ))G5c%d(lt|gt|eq|so) = ([^;]*);" % N,
                              lambda m: "%sG7CR_SET(%d, %s, %s);" % (m.group(1), N, m.group(2), m.group(3)), code)
            n["G7CR_SET"] = n.get("G7CR_SET", 0) + k
            if re.search(r"\bG5c%d(lt|gt|eq|so) [-+|&^]?= " % N, code):
                raise SystemExit("cr: escritura no reconocida: " + line[:200])
        out.append(uses(code) + cm)
        i += 1
    return out


def cr_transform(text, n):
    lines = text.split("\n")
    out, i, L = [], 0, len(lines)
    while i < L:
        if RE_G6FN.match(lines[i]):
            j = i + 1
            while j < L and lines[j] != "}":
                j += 1
            out.extend(cr_fn(lines[i:j + 1], n))
            i = j + 1
        else:
            out.append(lines[i]); i += 1
    return "\n".join(out)


def disp_transform(text, n):
    out, prev, k, skipped = [], "", 0, 0
    for line in text.split("\n"):
        if "nwii_dispatch(ctx); if (ctx.pc != 0x" in line:
            m = RE_DISP.match(line)
            ml = RE_SETLR.match(prev)
            if m and ml and ml.group(1) == m.group(4) and int(m.group(5), 16) == int(m.group(4), 16) - 4 \
                    and "ctx.lr" not in m.group(2) + m.group(3) and "ctx.pc" not in m.group(2) + m.group(3):
                R = m.group(4)
                line = ("%s%s%s{ uint64_t t_ = g7_bctrl_g6(ctx, G5ic, 0x%su, G5pc); if (NWII_UNLIKELY((int64_t)t_ < 0)) "
                        "return g6_exact(ctx, G6lr0); }%s" % (m.group(1), m.group(2), m.group(3), R, m.group(6)))
                k += 1
            else:
                skipped += 1
        if line.strip() and not line.lstrip().startswith("//") and not line.startswith("loc_"):
            prev = line
        out.append(line)
    n["disp"] = k
    n["disp no transformadas"] = skipped
    return "\n".join(out)


def transform(text, feats, is_export=True, impl=False):
    if MARK in text[:4000]:
        raise SystemExit("ya pasado por G7")
    n = {}
    if "disp" in feats:
        text = disp_transform(text, n)
    if "cr" in feats:
        text = cr_transform(text, n)
    for rx, rep in rules(feats):
        text, k = rx.subn(rep, text)
        n[rx.pattern] = k
    if is_export:
        inc = '#include "g6c_decls.h"  // [r23-g6c]'
        if inc not in text:
            raise SystemExit("falta el include de g6c_decls.h")
        text = text.replace(inc, inc + ('\n#define G7_IMPL 1  // [r33-g7] esta unidad define las funciones frias y los trampolines' if impl else '') + '\n#include "g7.h"  // [r33-g7]', 1)
    text = f"{MARK} compact hot code: {','.join(sorted(feats))} (r9/r33g7/tools/apply_g7.py)\n" + text
    return text, n


def one(args):
    path, feats = args
    with open(path) as f:
        t = f.read()
    t2, n = transform(t, feats, impl=os.path.basename(path) == 'output_0.cpp')
    with open(path, "w") as f:
        f.write(t2)
    return os.path.basename(path), n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("export", nargs="?")
    ap.add_argument("--feat", default=",".join(ALL))
    ap.add_argument("--file", nargs=2)
    ap.add_argument("--jobs", type=int, default=int(os.environ.get("G5_JOBS", "2")))
    a = ap.parse_args()
    feats = set(x for x in a.feat.split(",") if x)
    if a.file:
        t, n = transform(open(a.file[0]).read(), feats, is_export=False)
        t = '#define G7_IMPL 1\n#include "g7.h"  // [r33-g7]\n' + t
        open(a.file[1], "w").write(t)
        print(n)
        return
    files = sorted(f for f in os.listdir(a.export) if re.fullmatch(r"output_\d+\.cpp", f))
    assert len(files) == 87, len(files)
    tot = {}
    with ProcessPoolExecutor(a.jobs) as ex:
        for name, n in ex.map(one, [(os.path.join(a.export, f), feats) for f in files]):
            for k, v in n.items():
                tot[k] = tot.get(k, 0) + v
    shutil.copy(os.path.join(HERE, "g7.h"), os.path.join(a.export, "g7.h"))
    # tabla de entradas _g6 para g7_bctrl_g6 (la incluye g7.h en la unidad con G7_IMPL)
    with open(os.path.join(a.export, "g7_disp.inc"), "w") as f:
        for nm in open(os.path.join(a.export, "g6c_fns.txt")).read().split():
            m = re.search(r"([0-9A-F]{8})$", nm)
            if m:
                f.write("G7D(0x%su, %s)\n" % (m.group(1), nm))
    with open(os.path.join(a.export, "g7_feats.txt"), "w") as f:
        f.write(",".join(sorted(feats)) + "\n")
    for k, v in sorted(tot.items()):
        print(f"{v:8d}  {k}")


if __name__ == "__main__":
    main()
