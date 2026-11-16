#!/usr/bin/env python3
"""mk_order.py: orden L de m3 = el de m2 (r24m2/nx/order/g6e2.txt) + las secciones de libnwiiruntime.a que el
orden de r7f (r24m2/nx/ref/r7f_dk3drel.txt) pone y g6e2 no conoce (las del worker de render de R7), insertadas
justo después de PrepFrame, en el orden en que las pone r7f.   Uso: mk_order.py <g6e2.txt> <r7f.txt> <salida>"""
import sys
g6 = open(sys.argv[1]).read().split('\n')
r7 = open(sys.argv[2]).read().split('\n')
have = set(l.strip() for l in g6)
new = [l for l in r7 if 'libnwiiruntime.a:' in l and l.strip() not in have]
pf = [i for i, l in enumerate(g6) if 'PrepFrame' in l and 'libnwiiruntime.a:gx.o' in l]
assert len(pf) == 1, pf
out = g6[:pf[0] + 1] + new + g6[pf[0] + 1:]
open(sys.argv[3], 'w').write('\n'.join(out))
print('mk_order: %d secciones insertadas tras PrepFrame (línea %d)' % (len(new), pf[0] + 1))
for l in new: print('  ' + l.strip())
