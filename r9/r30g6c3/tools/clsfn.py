#!/usr/bin/env python3
"""clsfn.py --elf X profile_cpu.txt --cls 'ctx.inst_count ld' [--top 25]: qué funciones (y qué instrucciones)
se llevan las muestras de una clase de hostprof."""
import argparse, bisect, collections, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hostprof as H
ap = argparse.ArgumentParser(); ap.add_argument('--elf'); ap.add_argument('--cls', action='append'); ap.add_argument('--top', type=int, default=25)
ap.add_argument('--ins', type=int, default=0, help='show N hottest instructions of each class per function')
ap.add_argument('profile'); a = ap.parse_args()
syms = H.load_syms(a.elf); starts = [s[0] for s in syms]
fs = collections.Counter(); isamp = collections.Counter()
for c, off in H.parse_profile(a.profile):
    i = bisect.bisect_right(starts, off) - 1
    if i < 0 or off >= syms[i][0] + max(syms[i][1], 4): continue
    if H.GEN.search(syms[i][2]): fs[i] += c; isamp[off] += c
res = collections.Counter(); per = {}
for i, n in fs.most_common(400):
    a0, sz, name = syms[i]
    ins = H.disasm(a.elf, a0, a0 + sz); creg = H.ctx_reg(ins)
    for ad, (mn, ops) in ins.items():
        k = H.classify(mn, ops, creg)
        if k in a.cls and isamp.get(ad):
            res[(name, k)] += isamp[ad]; per.setdefault((name, k), []).append((isamp[ad], hex(ad), mn, ops))
for (name, k), v in res.most_common(a.top):
    print('%6d  %-22s %s' % (v, k, name))
    for x in sorted(per[(name, k)], reverse=True)[:a.ins]:
        print('         ', *x)
