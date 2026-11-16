#!/usr/bin/env python3
"""mk_order.py <in order.txt> <g6c_fns.txt> <out order.txt>
L order (r9/tools/hot_order.py) of G6 e2 -> G6 stage 3: each hot `.text.NAME` of a function that now
has a _g6 body gets `.text.NAME_g6` (the body, what runs on direct calls) right before it (the wrapper
NAME stays next to it). Everything else unchanged."""
import re, sys
src, fns, out = sys.argv[1:4]
g6 = set(l.split()[0] for l in open(fns) if l.strip())
res, n = [], 0
for l in open(src):
    m = re.match(r'^(\s*\*/output_\d+\.o\(\.text\.)(\w+)\)\s*$', l)
    if m and m.group(2) in g6:
        res.append('%s%s_g6)\n' % (m.group(1), m.group(2))); n += 1
    res.append(l)
open(out, 'w').writelines(res)
print('mk_order: %d _g6 bodies placed' % n)
