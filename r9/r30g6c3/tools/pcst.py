#!/usr/bin/env python3
"""pcst.py --elf X profile: dónde caen las muestras de las escrituras/lecturas de ctx.pc (antes de llamada / en retorno /
sondeo / comprobación tras llamada / entrada) en las 400 funciones generadas más calientes."""
import argparse, bisect, collections, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostprof as H
ap = argparse.ArgumentParser(); ap.add_argument('--elf'); ap.add_argument('profile'); a = ap.parse_args()
syms = H.load_syms(a.elf); starts = [s[0] for s in syms]
fs = collections.Counter(); isamp = collections.Counter()
for c, off in H.parse_profile(a.profile):
    i = bisect.bisect_right(starts, off) - 1
    if i < 0 or off >= syms[i][0] + max(syms[i][1], 4): continue
    if H.GEN.search(syms[i][2]): fs[i] += c; isamp[off] += c
res = collections.Counter(); tot = sum(fs.values())
for i, n in fs.most_common(400):
    a0, sz, name = syms[i]
    ins = H.disasm(a.elf, a0, a0 + sz); creg = H.ctx_reg(ins); addrs = sorted(ins)
    for j, ad in enumerate(addrs):
        s = isamp.get(ad, 0)
        if not s: continue
        k = H.classify(*ins[ad], creg)
        if not k.startswith(('ctx.pc', 'ctx.lr', 'ctx.inst_count')): continue
        nxt = [ins[x] for x in addrs[j + 1:j + 7]]
        prv = [ins[x] for x in addrs[max(0, j - 4):j]]
        kind = 'other'
        if any(m == 'ret' for m, _ in nxt): kind = 'near ret'
        elif any(m == 'bl' for m, _ in nxt): kind = 'before bl'
        elif any(m == 'bl' for m, _ in prv): kind = 'right after bl'
        elif any(H.classify(m, o, creg).startswith('ctx.poll') or 'poll' in o for m, o in nxt): kind = 'before poll'
        elif j < 12: kind = 'entry'
        res[(k, kind)] += s
for (k, kind), v in sorted(res.items(), key=lambda kv: -kv[1]):
    print('%6d  %5.2f%%  %-20s %s' % (v, 100.0 * v / tot, k, kind))
