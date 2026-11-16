#!/usr/bin/env python3
"""[r44w] gen_w_agua.py <export dir> <salida.cpp>

Genera el renderizador nativo de la superficie del agua (frente W, NWII_W_AGUA) a partir del CÓDIGO GENERADO exacto:
  - copia func_8000D000_g6 (output_1.cpp) y los dos thunks del REL func_807B6E50_g6 / func_807B6E60_g6
    (output_48.cpp, `b 8000D000`) con nombres w_agua_*;
  - dentro de los 5 bucles de vértices (rangos PPC de abajo) cambia cada escritura al pipe de GX
    (stb/sth/stfs a 0xCC008000 con base = r23/r28/r29/r30, cargada con `lis rX,0xcc01`) por un añadido a una
    cola local (W_PUT8/16/F32: bytes en big endian + una cinta con el tipo de cada escritura);
  - antes de cualquier sentencia de esos bucles que pueda salir de la secuencia recta (llamada, sondeo, return)
    y en la primera dirección de después de cada bucle, W_COMMIT() entrega la cola al runtime
    (GX_WGPIPE_CommitTape, gx.cpp), que deja el pipe y su estado (sniffer, BP armado, contador) EXACTAMENTE como
    lo habrían dejado las llamadas Write8/16/F32 una a una (o las repite una a una si no está en el camino rápido).
    Además W_COMMIT() antes de todo `return` de la función.
Todo lo demás (la preparación de GX, GXBegin, los sondeos, el protocolo g6) es el código generado tal cual.
"""
import re
import sys

EXP, OUT = sys.argv[1], sys.argv[2]
LOOPS = [(0x8000B89C, 0x8000BA48), (0x8000BE20, 0x8000BFE8), (0x8000C3C8, 0x8000C594),
         (0x8000C91C, 0x8000CA24), (0x8000CE00, 0x8000CFC8)]
EXITS = {e + 4 for _, e in LOOPS}
PIPE_BASES = {"G5r23", "G5r28", "G5r29", "G5r30"}


def extract(path, name):
    lines = open(path).read().split("\n")
    s = next(i for i, l in enumerate(lines) if l.startswith('extern "C" uint64_t %s(' % name))
    e = next(i for i in range(s + 1, len(lines)) if lines[i] == "}")
    return lines[s:e + 1]


def head(path):
    lines = open(path).read().split("\n")
    out = []
    for l in lines:
        if l.startswith('extern "C" uint64_t func_'):
            break
        out.append(l)
    return out


RX_STORE = re.compile(r"^(\s*)(ctx\.inst_count = G5ic; )?mv_\.(write8|write16|write_f32)_io\((G5r\d+) \+ -32768, (.+)\); // (.*)$")
SAFE = [re.compile(x) for x in (
    r"^loc_\w+: ;$", r"^\s*//", r"^\s*$", r"^\s*G7IC\(\d+\);$", r"^\s*\{$", r"^\s*\}( // \w+)?$",
    r"^\s*int32_t sv = ", r"^\s*uint32_t ca = ", r"^\s*G5ca = \(ca\);$",
    r"^\s*G7CR_CMP\w*\([^;]*\);$", r"^\s*if \(G7CRB\(\d, \w+\)\) goto loc_\w+;$",
    r"^\s*ctx\.fill\(\d+, \(double\)mv_\.read_f32<\d+u>\([^;]*\)\); // lfs$",
)]
RX_ASSIGN = re.compile(r"^\s*G5\w+ = [^;]*; // [^;]*$|^\s*G5\w+ = [^;]*;$")


def is_safe(l):
    if any(r.match(l) for r in SAFE):
        return True
    if RX_ASSIGN.match(l) and "ctx" not in l.split("//")[0] and "func_" not in l and "g6_" not in l:
        return True
    return False


def transform(lines, stats):
    out = []
    addr = None
    rx_addr = re.compile(r"^\s*// 0x([0-9A-F]{8}): ")
    rx_lab = re.compile(r"^loc_([0-9A-F]{8}): ;$")
    for i, l in enumerate(lines):
        m = rx_addr.match(l)
        if m:
            addr = int(m.group(1), 16)
        ml = rx_lab.match(l)
        if ml:
            a = int(ml.group(1), 16)
            out.append(l)
            if a in EXITS:
                out.append("    W_COMMIT();  // [r44w] fin del bucle de vértices")
                stats["exits"] += 1
            continue
        if i == 1 and "MemView mv_" in l:
            out.append(l)
            out.append("    W_DECL;  // [r44w] cola local de escrituras al pipe")
            continue
        inloop = addr is not None and any(s <= addr <= e for s, e in LOOPS)
        if inloop:
            ms = RX_STORE.match(l)
            if ms and ms.group(4) in PIPE_BASES:
                ind, ic, kind, base, val, com = ms.groups()
                mac = {"write8": "W_PUT8", "write16": "W_PUT16", "write_f32": "W_PUTF32"}[kind]
                out.append("%s%s%s(%s, %s);  // [r44w] %s" % (ind, ic or "", mac, base, val, com))
                stats[kind] += 1
                continue
            if not is_safe(l):
                out.append("    W_COMMIT();")
                stats["commits"] += 1
                out.append(l)
                continue
            out.append(l)
            continue
        if "return" in l and not l.lstrip().startswith("//"):
            out.append("    W_COMMIT();")
            stats["ret_commits"] += 1
        out.append(l)
    return out


def main():
    f1 = extract(EXP + "/output_1.cpp", "func_8000D000_g6")
    t50 = extract(EXP + "/output_48.cpp", "func_807B6E50_g6")
    t60 = extract(EXP + "/output_48.cpp", "func_807B6E60_g6")
    stats = {"write8": 0, "write16": 0, "write_f32": 0, "commits": 0, "ret_commits": 0, "exits": 0}
    body = transform(f1, stats)
    body = [l.replace("func_8000D000_g6", "w_agua_8000D000_g6") for l in body]
    t50 = [l.replace("func_8000D000_g6", "w_agua_8000D000_g6").replace("func_807B6E50_g6", "w_agua_807B6E50_g6") for l in t50]
    t60 = [l.replace("func_8000D000_g6", "w_agua_8000D000_g6").replace("func_807B6E60_g6", "w_agua_807B6E60_g6") for l in t60]
    o = ["// [r44w] GENERADO por r9/r44w/tools/gen_w_agua.py a partir de %s (NO editar a mano)" % EXP]
    o += head(EXP + "/output_1.cpp")
    o += ['#include "w_agua_cola.h"', 'extern "C" uint64_t w_agua_8000D000_g6(nwii::runtime::CPUContext& ctx, uint64_t G5ic_in, uint64_t G6lr0);', ""]
    o += body + [""] + t50 + [""] + t60 + [""]
    o += ['void w_agua_8000D000(CPUContext& ctx) { w_agua_8000D000_g6(ctx, ctx.inst_count, (uint64_t)ctx.lr | G6W | G6CHK); }',
          'void w_agua_807B6E50(CPUContext& ctx) { w_agua_807B6E50_g6(ctx, ctx.inst_count, (uint64_t)ctx.lr | G6W | G6CHK); }',
          'void w_agua_807B6E60(CPUContext& ctx) { w_agua_807B6E60_g6(ctx, ctx.inst_count, (uint64_t)ctx.lr | G6W | G6CHK); }', "",
          '#include "w_agua_hook.inc"', ""]
    open(OUT, "w").write("\n".join(o))
    print("gen_w_agua: %s" % stats)


if __name__ == "__main__":
    main()
