#!/usr/bin/env python3
"""sondeos.py <carpeta de corrida>... : por demo, media de las líneas [NX] game: sondeos lentos k/s, de ellos «clock»
(cuenta atrás agotada = refresco del reloj + agenda), full/tail, fps del juego, sondeos lentos por fotograma,
salida de audio (kHz), listas AX/s y underruns al final."""
import re, sys, collections

for d in sys.argv[1:]:
    cur = None
    acc = collections.defaultdict(list)
    und = None
    for l in open(d + "/nwii.log", errors="replace"):
        m = re.match(r"\[DEMO\] start (\S+)", l)
        if m:
            cur = m.group(1); continue
        if l.startswith("[DEMO] end"):
            cur = None; continue
        if cur and l.startswith("[NX] game"):
            g = re.search(r"game ([\d.]+) fps", l); s = re.search(r"slow polls (\d+)k/s \(full (\d+)/s, tail (\d+)/s, clock (\d+)k/s\)", l)
            a = re.search(r"out ([\d.]+) kHz", l); ax = re.search(r"ax thread (\d+) lists/s", l)
            if g and s and float(g.group(1)) > 5:
                acc[cur].append((float(g.group(1)), int(s.group(1)), int(s.group(2)), int(s.group(3)), int(s.group(4)),
                                 float(a.group(1)) if a else 0, int(ax.group(1)) if ax else 0))
        u = re.search(r"underruns (\d+)", l)
        if u: und = int(u.group(1))
    print(d.rstrip("/").split("/")[-1], f"(underruns al final: {und})")
    print(f"  {'demo':20s} {'fps':>5s} {'lentos k/s':>10s} {'clock k/s':>9s} {'full/s':>6s} {'tail/s':>6s} {'lentos/fot':>10s} {'kHz':>5s} {'AX/s':>5s}")
    for k, v in acc.items():
        n = len(v); f = sum(x[0] for x in v) / n; sl = sum(x[1] for x in v) / n
        print(f"  {k:20s} {f:5.1f} {sl:10.0f} {sum(x[4] for x in v)/n:9.0f} {sum(x[2] for x in v)/n:6.0f} "
              f"{sum(x[3] for x in v)/n:6.0f} {1000*sl/f:10.0f} {sum(x[5] for x in v)/n:5.1f} {sum(x[6] for x in v)/n:5.0f}")
