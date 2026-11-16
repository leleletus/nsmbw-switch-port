#!/usr/bin/env python3
"""leafchain.py <bench> <nm> <escena> <hoja> [hilo=0]: cadenas (pila del anfitrion) de las muestras cuya hoja es <hoja>."""
import collections, os, struct, sys
sys.path.insert(0, "/home/djvemo/nsmbw-port/r9/r31t/tools")
import tprof  # noqa
d, nm, esc, leafname = sys.argv[1:5]
slot = int(sys.argv[5]) if len(sys.argv) > 5 else 0
pr = tprof.Prof(d, nm, None, None)
sid = tprof.DEMOS.index(esc)
raw = open(os.path.join(d, "tprof_host.bin"), "rb").read()
h = list(struct.iter_unpack("<IIIIHbB", raw[:len(raw) // 20 * 20]))
c = collections.Counter(); n = 0
for r in h:
    if r[5] != sid or r[6] != slot: continue
    s0 = pr.hsym(r[1]) or "?"
    if s0 != leafname: continue
    n += 1
    chain = [pr.hsym(a) or "?" for a in (pr.stacks[r[3]][1:] if r[3] and r[3] < len(pr.stacks) else [])]
    c[(pr.hsym(r[2]) or "?",) + tuple(x[:60] for x in chain[:6])] += 1
print(n, "muestras")
for k, v in c.most_common(15): print(v, " <- ".join(k))
