#!/usr/bin/env python3
"""a64_static.py <elf> <salida.npz>: tabla estática de TODAS las instrucciones AArch64 de .text del ELF de Switch.

Índice i = dirección / 4. Guarda:
  cls[i]    clase de la instrucción (misma clasificación que r20g6/tools/hostprof.py; lista en 'clases')
  flg[i]    bit0 = empieza bloque básico, bit1 = acaba en salto incondicional (b/br/ret), bit2 = ret,
            bit3 = bl/blr, bit4 = salto condicional, bit5 = `b` a otra función (llamada de cola)
  tgt[i]    índice de la instrucción destino de un salto directo (-1 si no hay)
  fn[i]     índice de la función (símbolo de nm) que la contiene (-1 si ninguna)
  'funcs'   JSON: [nombre, dirección, tamaño]
JOBS procesos (3 por defecto), cada uno con su objdump sobre un trozo de .text.
"""
import bisect
import collections
import json
import os
import re
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

import numpy as np

DKP = os.environ.get("DEVKITPRO", "/opt/devkitpro")
NM = os.path.join(DKP, "devkitA64", "bin", "aarch64-none-elf-nm")
OD = os.path.join(DKP, "devkitA64", "bin", "aarch64-none-elf-objdump")
RE = os.path.join(DKP, "devkitA64", "bin", "aarch64-none-elf-readelf")
FIELDS = [(0, 128, "gpr"), (128, 384, "fpr"), (384, 640, "ps1"), (640, 672, "cr"), (672, 676, "pc"),
          (676, 680, "lr"), (680, 684, "ctr"), (684, 688, "xer"), (688, 692, "msr"), (692, 696, "fpscr"),
          (696, 704, "srr"), (704, 736, "gqr"), (856, 912, "mmu"), (912, 920, "inst_count"),
          (1244, 1248, "poll_left"), (1248, 1249, "poll_dec_due"), (1016, 1020, "pending_cb")]
CLASES = ["alu", "mov/const", "cmp/cset", "branch", "call", "ret", "bswap", "fp", "fcvt (round)", "stack ld", "stack st",
          "guest ld", "guest st", "other-mem ld", "other-mem st"] + \
         [f"ctx.{f} {k}" for f in ["gpr", "fpr", "ps1", "cr", "pc", "lr", "ctr", "xer", "msr", "fpscr", "srr", "gqr",
                                   "mmu(MemView)", "inst_count", "poll_left", "poll_dec_due", "pending_cb", "other"]
          for k in ("ld", "st")]
CI = {n: i for i, n in enumerate(CLASES)}
MEM = re.compile(r"\[(\w+)(?:, ?([^\]]+))?\](!?)")
CTXF = ("gpr", "fpr", "ps1", "cr", "pc", "lr", "ctr", "inst_count", "poll_left")


def field(off):
    for lo, hi, n in FIELDS:
        if lo <= off < hi:
            return n
    return "other" if off < 1320 else None


def classify(mn, ops, creg):
    if mn.startswith(("ld", "st")) and "[" in ops:
        kind = "ld" if mn.startswith("ld") else "st"
        m = MEM.search(ops)
        b, off = m.group(1), m.group(2)
        if b == "sp":
            return "stack " + kind
        if off and "uxtw" in off:
            return "guest " + kind
        if b == creg:
            v = 0
            if off and off.startswith("#"):
                try:
                    v = int(off[1:], 0)
                except ValueError:
                    v = 0
            f = field(v) or "other"
            if f == "mmu":
                f = "mmu(MemView)"
            return f"ctx.{f} {kind}"
        return "other-mem " + kind
    if mn in ("rev", "rev16", "rev32"):
        return "bswap"
    if mn in ("bl", "blr"):
        return "call"
    if mn == "ret":
        return "ret"
    if mn in ("b", "br") or mn.startswith(("b.", "cb", "tb")):
        return "branch"
    if mn in ("cmp", "cmn", "tst", "ccmp", "ccmn", "fcmp", "fcmpe", "cset", "csel", "csinc", "csinv", "csneg"):
        return "cmp/cset"
    if mn == "fcvt":
        return "fcvt (round)"
    if mn.startswith("f") or mn in ("scvtf", "ucvtf"):
        return "fp"
    if mn in ("mov", "movk", "movz", "movn", "adrp", "adr"):
        return "mov/const"
    return "alu"


def chunk(args):
    elf, lo, hi, fstarts, fends = args
    n = (hi - lo) // 4
    cls = np.zeros(n, dtype=np.uint8)
    flg = np.zeros(n, dtype=np.uint8)
    tgt = np.full(n, -1, dtype=np.int32)
    p = subprocess.Popen([OD, "-d", "--no-show-raw-insn", "-j", ".text", f"--start-address={lo:#x}",
                          f"--stop-address={hi:#x}", elf], stdout=subprocess.PIPE, text=True, bufsize=1 << 20)
    buf = []      # instrucciones de la función en curso: (idx, mn, ops)
    fi = bisect.bisect_right(fstarts, lo) - 1
    cur_end = fends[fi] if fi >= 0 else lo

    def flush():
        cnt = collections.Counter()
        for _, mn, ops in buf:
            if mn[0] in "ls" and mn.startswith(("ld", "st")):
                m = MEM.search(ops)
                if m and m.group(1) != "sp" and m.group(1).startswith("x"):
                    off = m.group(2)
                    if off is None:
                        cnt[m.group(1)] += 0.2
                    elif off.startswith("#"):
                        try:
                            v = int(off[1:], 0)
                        except ValueError:
                            continue
                        if field(v) in CTXF:
                            cnt[m.group(1)] += 1
        creg = cnt.most_common(1)[0][0] if cnt else None
        for i, mn, ops in buf:
            cls[i] = CI[classify(mn, ops, creg)]
        buf.clear()

    for line in p.stdout:
        if not line.startswith(" ") or ":\t" not in line:
            continue
        a, rest = line.split(":\t", 1)
        try:
            addr = int(a, 16)
        except ValueError:
            continue
        if addr >= cur_end:
            flush()
            fi = bisect.bisect_right(fstarts, addr) - 1
            cur_end = fends[fi] if fi >= 0 and addr < fends[fi] else (fstarts[fi + 1] if fi + 1 < len(fstarts) else hi)
        parts = rest.split("\t", 1)
        mn = parts[0].strip()
        ops = parts[1].split("//")[0] if len(parts) > 1 else ""
        i = (addr - lo) // 4
        f = 0
        if mn == "ret":
            f = 2 | 4
        elif mn == "br":
            f = 2
        elif mn in ("bl", "blr"):
            f = 8
            if mn == "bl":
                try:
                    tgt[i] = int(ops.split()[0], 16) // 4
                except (ValueError, IndexError):
                    pass
        elif mn == "b" or mn.startswith(("b.", "cb", "tb")):
            f = 2 if mn == "b" else 16
            try:
                t = int(ops.split("<")[0].split(",")[-1].strip(), 16)
                tgt[i] = t // 4
                if mn == "b" and fi >= 0 and not (fstarts[fi] <= t < fends[fi]):
                    f |= 32
            except ValueError:
                pass
        flg[i] = f
        buf.append((i, mn, ops.split("<")[0].strip()))
    flush()
    p.wait()
    return lo, cls, flg, tgt


def main():
    elf, dst = sys.argv[1], sys.argv[2]
    tsize = 0
    for ln in subprocess.run([RE, "-S", "-W", elf], capture_output=True, text=True).stdout.splitlines():
        m = re.match(r"\s*\[\s*\d+\] \.text\s+PROGBITS\s+([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)", ln)
        if m:
            tsize = int(m.group(2), 16)
    syms = []
    for ln in subprocess.run([NM, "-n", "-S", "--defined-only", elf], capture_output=True, text=True).stdout.splitlines():
        q = ln.split()
        if len(q) == 4 and q[2] in "tTwW" and int(q[0], 16) < tsize:
            syms.append((int(q[0], 16), int(q[1], 16), q[3]))
    # sin solapes: una función por dirección de inicio (la primera)
    funcs, last = [], -1
    for a, s, nme in syms:
        if a != last and s > 0:
            funcs.append([nme, a, s])
            last = a
    fstarts = [f[1] for f in funcs]
    fends = [f[1] + f[2] for f in funcs]
    n = tsize // 4
    J = int(os.environ.get("JOBS", "3"))
    cuts = [0]
    for k in range(1, J * 4):
        a = fstarts[bisect.bisect_left(fstarts, tsize * k // (J * 4))] if tsize * k // (J * 4) < fstarts[-1] else tsize
        if a > cuts[-1]:
            cuts.append(a)
    cuts.append(tsize & ~3)
    cls = np.zeros(n, dtype=np.uint8)
    flg = np.zeros(n, dtype=np.uint8)
    tgt = np.full(n, -1, dtype=np.int32)
    with ProcessPoolExecutor(max_workers=J) as ex:
        for lo, c, f, t in ex.map(chunk, [(elf, cuts[k], cuts[k + 1], fstarts, fends) for k in range(len(cuts) - 1)]):
            i0 = lo // 4
            cls[i0:i0 + len(c)], flg[i0:i0 + len(c)], tgt[i0:i0 + len(c)] = c, f, t
            print(f"trozo {lo:#x} listo", flush=True)
    fn = np.full(n, -1, dtype=np.int32)
    for k, (nme, a, s) in enumerate(funcs):
        fn[a // 4:(a + s) // 4] = k
    # líderes: inicio de función, destino de salto directo (no bl), instrucción tras cualquier salto o ret
    lead = np.zeros(n, dtype=bool)
    lead[np.array(fstarts) // 4] = True
    isbr = (flg & (2 | 16)) != 0
    lead[1:] |= isbr[:-1]
    t = tgt[isbr & (tgt >= 0) & (tgt < n)]
    lead[t] = True
    flg |= lead.astype(np.uint8)
    np.savez_compressed(dst, cls=cls, flg=flg, tgt=tgt, fn=fn, funcs=json.dumps(funcs), clases=json.dumps(CLASES))
    print(n, "instrucciones,", len(funcs), "funciones,", int(lead.sum()), "bloques")


if __name__ == "__main__":
    main()
