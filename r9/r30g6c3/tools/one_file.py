#!/usr/bin/env python3
"""one_file.py <in output_N.cpp> <out dir> : --proto 3 pass on one file (G6_FNS from c3/export/g6c_fns.txt),
for quick looks/compiles; writes <out dir>/output_N.cpp + g6c_decls.h + functions.h."""
import os, re, sys, shutil
inp, od = sys.argv[1], sys.argv[2]
H = os.path.dirname(os.path.abspath(__file__)); C = os.path.dirname(H)
sys.argv = ['x']; sys.path.insert(0, H)
import apply_g6c as A
A.PROTO = 3; A.LAZY = False; A.EAGER_ENTRY = True
A.REF = re.compile(A.REF_BASE + r'|ctx\.pc\b|ctx\.lr\b')
A.EXCLUDE.update(x.strip() for x in open(H + '/g5_exclude_g6b.txt') if x.strip() and not x.startswith('#'))
d = C + '/../r20g6/e2/export_pre'
A.collect_leaves(sorted(os.path.join(d, f) for f in os.listdir(d) if re.fullmatch(r'output_\d+\.cpp', f)))
A.G6_FNS.update(l.split()[0] for l in open(C + '/c3/export/g6c_fns.txt'))
A.G6_PHASE = 2
os.makedirs(od, exist_ok=True)
with open(od + '/g6c_decls.h', 'w') as f:
    f.write('#pragma once\n#include "runtime/cpu_context.h"\n' + '\n'.join(A.decl_head() + [A.decl(x) for x in sorted(A.G6_FNS)]) + '\n')
shutil.copy(d + '/functions.h', od)
shutil.copy(inp, od + '/' + os.path.basename(inp))
print(A.one_file(od + '/' + os.path.basename(inp)))
