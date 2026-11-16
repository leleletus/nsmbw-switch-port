#!/usr/bin/env python3
"""Call-graph helper for Wii DOL files.

Usage:
  dolcg.py <main.dol> api <lo> <hi> [sdk_lo sdk_hi]   functions in [lo,hi) called from outside the SDK range
  dolcg.py <main.dol> callees <addr>                  bl targets inside the function at addr
  dolcg.py <main.dol> callers <addr>                  call sites of addr
  dolcg.py <main.dol> dis <addr> [count]              disassemble count instructions
"""
import bisect
import struct
import sys

import capstone


def load_dol(path):
    data = open(path, "rb").read()
    offs = struct.unpack(">18I", data[0x00:0x48])
    addrs = struct.unpack(">18I", data[0x48:0x90])
    sizes = struct.unpack(">18I", data[0x90:0xD8])
    text = []
    for i in range(7):
        if sizes[i]:
            text.append((addrs[i], data[offs[i]:offs[i] + sizes[i]]))
    return text


def build(text):
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_PS)
    md.skipdata = True
    calls = []  # (site, target, is_tail_branch)
    starts = set()
    for base, blob in text:
        for i in range(0, len(blob), 4):
            w = struct.unpack(">I", blob[i:i + 4])[0]
            site = base + i
            if (w >> 26) == 18:  # b / bl
                li = w & 0x03FFFFFC
                if li & 0x02000000:
                    li -= 0x04000000
                target = (li if (w & 2) else site + li) & 0xFFFFFFFF
                if w & 1:
                    calls.append((site, target, False))
                    starts.add(target)
                else:
                    calls.append((site, target, True))
            # stwu r1,-X(r1) marks most prologues
            if (w & 0xFFFF0000) == 0x94210000 and (w & 0x8000):
                starts.add(site)
    return md, calls, sorted(starts)


def func_of(starts, addr):
    i = bisect.bisect_right(starts, addr) - 1
    return starts[i] if i >= 0 else None


def read_insns(md, text, addr, count):
    for base, blob in text:
        if base <= addr < base + len(blob):
            off = addr - base
            return list(md.disasm(blob[off:off + count * 4], addr))
    return []


def main():
    path, cmd = sys.argv[1], sys.argv[2]
    text = load_dol(path)
    md, calls, starts = build(text)
    h = lambda s: int(s, 16)
    if cmd == "api":
        lo, hi = h(sys.argv[3]), h(sys.argv[4])
        slo = h(sys.argv[5]) if len(sys.argv) > 5 else 0x801A0000
        shi = h(sys.argv[6]) if len(sys.argv) > 6 else 0x80240000
        res = {}
        for site, tgt, tail in calls:
            if lo <= tgt < hi and not (slo <= site < shi) and not tail:
                res.setdefault(tgt, []).append(site)
        for tgt in sorted(res):
            nxt = starts[bisect.bisect_right(starts, tgt)] if bisect.bisect_right(starts, tgt) < len(starts) else tgt
            print(f"{tgt:08x} size~{nxt - tgt:#x} calls={len(res[tgt])} from " + " ".join(f"{s:08x}" for s in res[tgt][:6]))
    elif cmd == "callees":
        a = h(sys.argv[3])
        nxt = starts[bisect.bisect_right(starts, a)]
        for site, tgt, tail in calls:
            if a <= site < nxt:
                print(f"{site:08x} {'b ' if tail else 'bl'} {tgt:08x}")
    elif cmd == "callers":
        a = h(sys.argv[3])
        for site, tgt, tail in calls:
            if tgt == a:
                print(f"{site:08x} in func {func_of(starts, site):08x} {'(tail)' if tail else ''}")
    elif cmd == "dis":
        a = h(sys.argv[3])
        n = int(sys.argv[4]) if len(sys.argv) > 4 else 40
        for ins in read_insns(md, text, a, n):
            print(f"{ins.address:08x}  {ins.mnemonic:8s} {ins.op_str}")


if __name__ == "__main__":
    main()
