#!/usr/bin/env python3
"""d40.py <bench|pc_run>...: por demo, suma de las lineas [D40] (lotes con varios finales de fotograma): lotes por el
camino barato y por el viejo, fotogramas tirados, ordenes por lote, conservadas por lote y us (pared) por lote."""
import re, sys, os
R = re.compile(r"barato (\d+) \(fot. tirados (\d+), ordenes (\d+), conservadas (\d+), ([0-9.]+) us/lote\) \| viejo (\d+) \(fot. tirados (\d+), ordenes (\d+), ([0-9.]+) us/lote\)")
for d in sys.argv[1:]:
    d = d.rstrip("/"); log = d + "/nwii.log" if os.path.exists(d + "/nwii.log") else d + "/run.log"
    acc = {}; demo = "-"
    for l in open(log, errors="replace"):
        if l.startswith("[DEMO] start "): demo = l.split()[2]; continue
        m = R.search(l)
        if not m: continue
        b, bf, bo, bk, bus, v, vf, vo, vus = [float(x) for x in m.groups()]
        a = acc.setdefault(demo, [0] * 8)
        a[0] += b; a[1] += bf; a[2] += bo; a[3] += bk; a[4] += bus * b
        a[5] += v; a[6] += vf; a[7] += vus * v
    print("== " + os.path.basename(d))
    for k, a in acc.items():
        print("  %-18s barato: lotes %5d tirados %5d ord/lote %6.0f cons/lote %5.0f us/lote %6.0f | viejo: lotes %5d tirados %5d us/lote %6.0f"
              % (k, a[0], a[1], a[2] / a[0] if a[0] else 0, a[3] / a[0] if a[0] else 0, a[4] / a[0] if a[0] else 0,
                 a[5], a[6], a[7] / a[5] if a[5] else 0))
