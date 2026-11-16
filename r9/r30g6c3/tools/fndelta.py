#!/usr/bin/env python3
"""fndelta.py A=elf:profile B=elf:profile ... : muestras por función (F, F_g6 y el envoltorio juntos),
como fracción de todas las muestras del hilo; imprime las mayores diferencias de cada build contra la 1.ª."""
import bisect, collections, subprocess, sys, re
NM = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm'
def syms(elf):
    out = subprocess.run([NM, '-n', '-S', '--defined-only', elf], capture_output=True, text=True).stdout
    s = []
    for l in out.splitlines():
        q = l.split()
        if len(q) == 4 and q[2] in 'tTwW': s.append((int(q[0], 16), int(q[1], 16), q[3]))
    return s
def prof(elf, path):
    S = syms(elf); st = [x[0] for x in S]; base = None; c = collections.Counter(); tot = 0; g6 = collections.Counter()
    for l in open(path):
        p = l.split()
        if not p or p[0].startswith('#'): continue
        if p[0] == 'base': base = int(p[1], 16); continue
        if len(p) == 3 and p[0].isdigit():
            n = int(p[0]); off = int(p[1], 16) - base; tot += n
            i = bisect.bisect_right(st, off) - 1
            name = S[i][2] if i >= 0 and off < S[i][0] + max(S[i][1], 4) else '?'
            if name.endswith('_g6'): name = name[:-3]; g6[name] += n
            c[name] += n
    return c, tot, g6
builds = []
for a in sys.argv[1:]:
    lab, rest = a.split('=', 1); elf, p = rest.split(':', 1)
    c, tot, g6 = prof(elf, p); builds.append((lab, c, tot, g6))
    print(lab, 'total samples', tot)
base = builds[0]
for lab, c, tot, g6 in builds[1:]:
    names = set(c) | set(base[1])
    d = {n: c.get(n, 0) / tot - base[1].get(n, 0) / base[2] for n in names}
    print('\n== %s - %s (puntos por mil de las muestras del hilo)' % (lab, base[0]))
    print('suma de |diff| %.2f, suma diff generadas %+.2f' % (1000*sum(abs(v) for v in d.values()),
          1000*sum(v for n, v in d.items() if n.startswith('func_') or re.search(r'_8[0-9A-F]{7}$', n))))
    for n in sorted(d, key=lambda n: -abs(d[n]))[:40]:
        print('  %+7.2f  %-50s %6.2f -> %6.2f   (en _g6: %d de %d)' % (1000*d[n], n[:50], 1000*base[1].get(n,0)/base[2], 1000*c.get(n,0)/tot, g6.get(n,0), c.get(n,0)))
