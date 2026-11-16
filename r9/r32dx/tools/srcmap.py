#!/usr/bin/env python3
"""srcmap.py: mapa estático del código generado partido (r32dx/src) -> datos/srcmap/.

Por archivo output_N.cpp (datos/srcmap/N.npz), con índice = número de línea:
  cls[línea]  clase de la sentencia (ver CLASES)      fn[línea]  índice de la función que la contiene (-1 fuera)
  ppc_line / ppc_addr / ppc_word / ppc_end: por cada instrucción PPC (comentario "// 0xADDR: WORD"): su línea,
  dirección, palabra de máquina y la línea donde acaban sus sentencias (siguiente comentario, etiqueta o fin).
  tok_line / tok_id: líneas que usan una función del runtime de la lista TOKS (accesos a memoria, MMIO, psq...)
  disp_line: líneas `nwii_dispatch(ctx);` (llamada indirecta por la tabla)
  call_line / call_fn: líneas con llamada directa `F(ctx);` y el índice de la función llamada (-1 si no es del juego)
datos/srcmap/funcs.json: [nombre, N, línea inicial, línea final, nº de instrucciones PPC, dirección invitada].
"""
import json
import os
import re
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np

V = "/home/djvemo/nsmbw-port/r9/r32dx"
CLASES = ["nada", "trabajo", "volcado", "recarga", "entrada_carga", "ic", "pc_lr", "entrada_switch", "sondeo",
          "sondeo_lento", "llamada", "despacho", "return"]
C = {n: i for i, n in enumerate(CLASES)}

TOKS = ["write8_io", "write16_io", "write32_io", "write_f32_io", "read16_io", "read32_io", "psq_st_io", "psq_l_fast",
        "psq_st_fast", "ppc_to_single", "resume_pruned", "micro_interpret", "interpret_step", "idle_wait(",
        "process_pending_callbacks_slow", "read_f32<", "write_f32<", "read32<", "write32<", "read8<", "write8<",
        "read16<", "write16<", "read_f64<", "write_f64<"]
RE_FN = re.compile(r"^void (\w+)\(CPUContext& ctx\) \{")
RE_PPC = re.compile(r"^\s*// 0x([0-9A-Fa-f]{8}): ([0-9A-Fa-f]{8})")
RE_LABEL = re.compile(r"^loc_[0-9A-Fa-f]+\w*: ;")
RE_FLUSH = re.compile(r"^ctx\.(gpr\[\d+\]|cr\[\d\]\.\w+|ctr|xer) = G5\w+;")
RE_RELOAD = re.compile(r"^G5(?:r(\d+) = ctx\.gpr\[\1\]|c(\d)(\w\w) = ctx\.cr\[\2\]\.\3|ctr = ctx\.ctr|xer = ctx\.xer);")
RE_DECL = re.compile(r"^(?:uint32_t|bool|uint64_t) G5\w+ = ctx\.")
RE_IC = re.compile(r"^(?:G5ic \+= \d+;|ctx\.inst_count = G5ic;|G5ic = ctx\.inst_count;|ctx\.inst_count \+= \d+;)")
RE_PC = re.compile(r"^(?:ctx\.pc = (?:0x[0-9A-Fa-f]+|ctx\.lr|ctx\.ctr);|ctx\.lr = 0x[0-9A-Fa-f]+;|if \(ctx\.pc != 0x[0-9A-Fa-f]+\) return;)")
RE_ENTRY = re.compile(r"^(?:if \(ctx\.pc != 0x[0-9A-Fa-f]+\) \{|switch \(ctx\.pc\) \{|case 0x[0-9A-Fa-f]+:|default:)")
RE_POLL = re.compile(r"^if \(NWII_UNLIKELY\(nwii::runtime::poll_due\(ctx\)\)\) \{")
RE_CALL = re.compile(r"^(\w+)\(ctx\);")
RE_DISP = re.compile(r"^(?:nwii_dispatch\(ctx\);|if \(ctx\.ctr\) \{|\} else nwii::runtime::note_null_call|else nwii::runtime::note_null_call|nwii::runtime::note_null_call)")


def do_file(n):
    path = os.path.join(V, "src", f"output_{n}.cpp")
    lines = open(path, errors="replace").read().split("\n")
    nl = len(lines) + 2
    cls = np.zeros(nl, dtype=np.uint8)
    fn = np.full(nl, -1, dtype=np.int32)
    funcs, ppc, calls, disp, toks = [], [], [], [], []
    cur, poll_depth, depth = -1, None, 0
    for i, raw in enumerate(lines, 1):
        m = RE_FN.match(raw)
        if m:
            cur = len(funcs)
            funcs.append([m.group(1), n, i, i, 0])
            fn[i] = cur
            poll_depth = None
            depth = 1
            continue
        if cur < 0:
            continue
        if raw == "}":
            funcs[cur][3] = i
            fn[i] = cur
            cur = -1
            continue
        fn[i] = cur
        m = RE_PPC.match(raw)
        if m:
            if ppc and ppc[-1][3] == 0:
                ppc[-1][3] = i
            ppc.append([i, int(m.group(1), 16), int(m.group(2), 16), 0])
            funcs[cur][4] += 1
            continue
        if RE_LABEL.match(raw):
            if ppc and ppc[-1][3] == 0:
                ppc[-1][3] = i
            continue
        s = raw.strip()
        if not s or s.startswith("//"):
            continue
        if "mv_." in s or "nwii::runtime::" in s:
            for ti, tk in enumerate(TOKS):
                if tk in s:
                    toks.append((i, ti))
        # profundidad de llaves (para saber dónde acaba el bloque lento del sondeo)
        code = s.split("//")[0]
        opens, closes = code.count("{"), code.count("}")
        if poll_depth is not None:
            cls[i] = C["sondeo_lento"]
            depth += opens - closes
            if depth <= poll_depth:
                poll_depth = None
            continue
        if RE_POLL.match(s):
            cls[i] = C["sondeo"]
            poll_depth = depth
            depth += opens - closes
            continue
        depth += opens - closes
        if s in ("{", "}"):
            continue
        if RE_IC.match(s):
            cls[i] = C["ic"]
        elif RE_FLUSH.match(s):
            cls[i] = C["volcado"]
        elif RE_RELOAD.match(s):
            cls[i] = C["recarga"]
        elif RE_DECL.match(s) or s.startswith("nwii::runtime::MemView mv_"):
            cls[i] = C["entrada_carga"]
        elif RE_PC.match(s):
            cls[i] = C["pc_lr"]
        elif RE_ENTRY.match(s):
            cls[i] = C["entrada_switch"]
        elif RE_DISP.match(s):
            cls[i] = C["despacho"]
            if s.startswith("nwii_dispatch(ctx);"):
                disp.append(i)
        elif s == "return;":
            cls[i] = C["return"]
        else:
            m = RE_CALL.match(s)
            if m:
                cls[i] = C["llamada"]
                calls.append((i, m.group(1)))
            else:
                cls[i] = C["trabajo"]
    for p in ppc:
        if p[3] == 0:
            p[3] = nl
    # una instrucción PPC acaba también donde acaba su función
    return n, cls, fn, funcs, ppc, calls, disp, toks


def main():
    os.makedirs(os.path.join(V, "datos", "srcmap"), exist_ok=True)
    res = {}
    with ProcessPoolExecutor(max_workers=int(os.environ.get("JOBS", "3"))) as ex:
        for r in ex.map(do_file, range(87)):
            res[r[0]] = r
    allf, base = [], {}
    for n in range(87):
        base[n] = len(allf)
        for f in res[n][3]:
            m = re.search(r"([0-9A-Fa-f]{8})$", f[0])
            allf.append(f + [int(m.group(1), 16) if m else 0])
    idx = {f[0]: i for i, f in enumerate(allf)}
    for n in range(87):
        _, cls, fn, funcs, ppc, calls, disp, toks = res[n]
        fn = np.where(fn >= 0, fn + base[n], -1).astype(np.int32)
        p = np.array(ppc, dtype=np.int64).reshape(-1, 4)
        # límite por función: la instrucción no pasa de la última línea de su función
        np.savez_compressed(os.path.join(V, "datos", "srcmap", f"{n}.npz"), cls=cls, fn=fn,
                            ppc_line=p[:, 0].astype(np.int32), ppc_addr=p[:, 1].astype(np.uint32),
                            ppc_word=p[:, 2].astype(np.uint32), ppc_end=p[:, 3].astype(np.int32),
                            disp_line=np.array(disp, dtype=np.int32),
                            tok_line=np.array([t[0] for t in toks], dtype=np.int32),
                            tok_id=np.array([t[1] for t in toks], dtype=np.int16),
                            call_line=np.array([c[0] for c in calls], dtype=np.int32),
                            call_fn=np.array([idx.get(c[1], -1) for c in calls], dtype=np.int32))
    json.dump(allf, open(os.path.join(V, "datos", "srcmap", "funcs.json"), "w"))
    json.dump(CLASES, open(os.path.join(V, "datos", "srcmap", "clases.json"), "w"))
    json.dump(TOKS, open(os.path.join(V, "datos", "srcmap", "toks.json"), "w"))
    print(len(allf), "funciones,", sum(len(res[n][4]) for n in range(87)), "instrucciones PPC")


if __name__ == "__main__":
    main()
