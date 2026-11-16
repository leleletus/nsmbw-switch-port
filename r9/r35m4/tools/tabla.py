#!/usr/bin/env python3
# tabla.py <bench_dir>... : tabla por escena (fps juego, fps dibujados, ms juego/fot REAL, render ms/fot) con
# r31t/tools/por_escena_real.py, + imagen (verif_imagen.txt) y linea [S2] final.
import subprocess, sys, re, os
R = os.path.expanduser('~/nsmbw-port/r9/r31t/tools/por_escena_real.py')
out = subprocess.run(['python3', R] + sys.argv[1:], capture_output=True, text=True).stdout
for l in out.splitlines():
    if l.startswith('==') or not l.strip(): print(l); continue
    if l.startswith('corrida'): print('%-34s %7s %7s %8s %8s' % ('corrida', 'fpsJ', 'fpsDib', 'msJ/fot', 'render')); continue
    p = l.split()
    print('%-34s %7s %7s %8s %8s' % (p[0][6:40], p[2], p[1], p[4], p[6]))
for d in sys.argv[1:]:
    v = open(d + '/verif_imagen.txt').read().strip().splitlines()[-1] if os.path.exists(d + '/verif_imagen.txt') else '?'
    s2 = ''
    for l in open(d + '/nwii.log', errors='replace'):
        if l.startswith('[S2] ACTIVO'): s2 = re.search(r'total:.*', l).group(0)
    print('%-34s %s | %s' % (os.path.basename(d.rstrip('/'))[6:40], v, s2))
