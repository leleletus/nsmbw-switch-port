#!/usr/bin/env python3
"""r28d1 (front D1): hooks in output_21.cpp (r22r7 export = r18m export) for the native search functions.
  - func_80162C30 (fLiMgBa_c::searchNodeByID) and func_80162D00 (fManager_c::searchBaseByID): one N1Slot hook
    line at the top (as r19n/tools/apply_n1.py), bodies in nWiiRuntime/src/hle/native_d1.cpp
  - d1_touch(this, node[, prev]) right after every label of cLineMg_c addTop/addLast/insert/removeLineNode
    (the functions and their merged copies), for the coherence of the ID index
Usage: apply_d1.py <export_dir>   (refuses to run twice; checks the sha256 of the clean file)"""
import hashlib, re, sys
p = sys.argv[1].rstrip('/') + '/output_21.cpp'
CLEAN = None  # filled below
s = open(p).read()
if '[d1]' in s:
    sys.exit('apply_d1: already applied')
h = hashlib.sha256(s.encode()).hexdigest()
print('apply_d1: sha256 of the clean output_21.cpp', h)
inc = '#include "functions.h"\n'
assert s.count(inc) == 1
s = s.replace(inc, inc + '#include "runtime/native_n1.h"  // [d1]\n#include "runtime/native_d1.h"  // [d1]\n'
              'namespace nwii { namespace runtime { bool d1h_80162C30(CPUContext &); bool d1h_80162D00(CPUContext &); bool d1h_80163090(CPUContext &); } }  // [d1]\n')
for addr, name in (('80162C30', 'SEARCHNODEBYID'), ('80162D00', 'SEARCHBASEBYID'), ('80163090', 'SEARCHNODEBYPROFNAME')):
    head = 'void func_%s(CPUContext& ctx) {\n' % addr
    assert s.count(head) == 1, addr
    slot = ('void func_%s(CPUContext& ctx);  // [d1]\n'
            'static nwii::runtime::N1Slot d1s_%s = {0x%su, "%s", &nwii::runtime::d1h_%s, &func_%s, 0u};  // [d1]\n'
            % (addr, addr, addr, name, addr, addr))
    hook = ('    if (ctx.pc == 0x%s && nwii::runtime::n1_hook(d1s_%s, ctx)) return;  // [d1]\n' % (addr, addr))
    s = s.replace(head, slot + head + hook)
labels = {'80161020': 'G5r3, G5r4', '80160FE0': 'G5r3, G5r4', '80160F60': 'G5r3, G5r4', '80160F10': 'G5r3, G5r4, G5r5'}
n = 0
for addr, args in labels.items():
    lab = 'loc_%s: ;\n' % addr
    cnt = s.count('\n' + lab)
    assert cnt >= 1, addr
    s = s.replace('\n' + lab, '\n' + lab + '    nwii::runtime::d1_touch(%s);  // [d1] cLineMg_c list operation\n' % args)
    n += cnt
s = '// [d1] r9/r28d1/tools/apply_d1.py: native searchNodeByID/searchBaseByID/searchNodeByProfName + %d list-operation hooks\n' % n + s
open(p, 'w').write(s)
print('apply_d1: 3 hooks, %d list-operation hooks' % n)
