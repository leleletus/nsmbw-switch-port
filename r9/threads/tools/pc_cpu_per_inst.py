#!/usr/bin/env python3
"""PC: CPU del hilo del juego por instrucción del juego, de las líneas [FPS] de un run.log.
Uso: pc_cpu_per_inst.py <run.log> [map|lvl11|N]  (cuenta las líneas [FPS] desde 2 después de llegar a la
escena: "phase 1 starts" = mapa, "phase 2 starts" = 1-1; o desde la línea [FPS] número N)
Saca: fps media, cpu thread %, host thread %, M instr./s, ns de CPU del hilo del juego por instrucción
(cpu% * 1e7 / (Minst * 1e6)), y el reparto de wgp/ax si está en la línea."""
import re, sys
rows = []
arg = sys.argv[2] if len(sys.argv) > 2 else 'map'
want = {'map': 'phase 1 starts', 'lvl11': 'phase 2 starts'}.get(arg)
seen = want is None
for l in open(sys.argv[1], errors='replace'):
    if not seen and want in l:
        seen = True
    if not l.startswith('[FPS]') or not seen:
        continue
    m = re.search(r'\[FPS\] ([\d.]+) presented.*guest ([\d.]+) Minst/s, host thread (\d+)%, cpu thread (\d+)%', l)
    if not m:
        continue
    w = re.search(r'wgp (\d+)k writes/s (\d+)k flushes/s', l)
    a = re.search(r'ax (\w+) (\d+) lists/s mix ([\d.]+) us', l)
    rows.append((float(m.group(1)), float(m.group(2)), int(m.group(3)), int(m.group(4)),
                 (int(w.group(1)), int(w.group(2))) if w else None, (a.group(1), float(a.group(3))) if a else None))
skip = 2 if want else int(arg)
r = rows[skip:]
if not r:
    print('no [FPS] rows'); sys.exit(1)
n = len(r)
fps = sum(x[0] for x in r) / n
mi = sum(x[1] for x in r) / n
ht = sum(x[2] for x in r) / n
ct = sum(x[3] for x in r) / n
ns = ct * 1e7 / (mi * 1e6)
extra = ''
ws = [x[4] for x in r if x[4]]
if ws:
    extra += ' wgp %.0fk writes/s %.0fk flushes/s' % (sum(a for a, _ in ws) / len(ws), sum(b for _, b in ws) / len(ws))
ax = [x[5] for x in r if x[5]]
if ax:
    extra += ' ax %s mix %.1f us' % (ax[-1][0], sum(b for _, b in ax) / len(ax))
print('n=%d fps %.1f  cpu thread %.1f%%  host thread %.1f%%  guest %.1f Minst/s  -> %.2f ns cpu/inst%s' % (n, fps, ct, ht, mi, ns, extra))
