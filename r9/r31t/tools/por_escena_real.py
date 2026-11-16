#!/usr/bin/env python3
"""por_escena_real.py <bench_dir>...: como tools/switchctl/por_escena.py pero con los fotogramas DEL JUEGO.

La linea "[NX] game X fps" cuenta fotogramas DIBUJADOS por el hilo de render (g_stat_frames). Cuando el render
no da abasto y descarta fotogramas (desierto4j, agua, castillo), "ms juego/fot" = cpu% x 1000/fps sale inflado.
Aqui los fotogramas del juego salen de las lineas [PIPE] ("game frames=N": copias XFB del juego cada ~2 s) y el
% del hilo del juego de las lineas [NX] de la misma escena (se salta la primera ventana de 5 s: carga).
Columnas: fps dibujados, fps del juego, ms de trabajo del hilo del juego por fotograma DEL JUEGO, lo que tarda el
render en dibujar un fotograma ([PIPE] draw avg), descartados/s, duracion de la demo (numero fijo de fotogramas
del juego: si baja, el juego va mas rapido)."""
import re, sys
from collections import defaultdict
NX = re.compile(r"game ([0-9.]+) fps.*threads main ([0-9]+)%, cpu ([0-9]+)%.*gxw ([0-9]+)%")
PIPE = re.compile(r"\[PIPE\] game frames=(\d+) interval avg=([0-9.]+).*draw avg=([0-9.]+) max=[0-9.]+ ms \(n=(\d+)\)")
def per_scene(path):
    sc = defaultdict(lambda: dict(nx=[], gf=0, gint=0.0, draw=0.0, rn=0, dur=0.0, npipe=0))
    cur, skip = None, 0
    for l in open(path, errors="replace"):
        if l.startswith("[DEMO] start "):
            cur, skip, skipp = l.split()[2], 1, 2
        elif l.startswith("[DEMO] end "):
            m = re.search(r"\(([0-9.]+) s\)", l)
            if cur and m: sc[cur]["dur"] += float(m.group(1))
            cur = None
        elif cur and l.startswith("[NX] game"):
            m = NX.search(l)
            if m:
                if skip: skip -= 1
                else: sc[cur]["nx"].append(tuple(float(x) for x in m.groups()))
        elif cur and l.startswith("[PIPE]"):
            m = PIPE.search(l)
            if m:
                if skipp: skipp -= 1; continue
                d = sc[cur]; n = int(m.group(1))
                d["gf"] += n; d["gint"] += n * float(m.group(2)); d["draw"] += float(m.group(3)) * int(m.group(4)); d["rn"] += int(m.group(4)); d["npipe"] += 1
    return sc
runs = [(d.rstrip("/").split("/")[-1], per_scene(d.rstrip("/") + "/nwii.log")) for d in sys.argv[1:]]
ids = []
for _, sc in runs:
    for k in sc:
        if k not in ids: ids.append(k)
for sid in ids:
    print(f"\n== {sid}")
    print(f"{'corrida':36s} {'fps dib':>7s} {'fps juego':>9s} {'juego%':>6s} {'ms juego/fot REAL':>17s} {'(formula vieja)':>15s} {'render ms/fot':>13s} {'nucleo2%':>8s} {'demo s':>7s}")
    for name, sc in runs:
        d = sc.get(sid)
        if not d or not d["nx"] or not d["gf"]:
            print(f"{name[:36]:36s}   -"); continue
        n = len(d["nx"]); avg = lambda i: sum(x[i] for x in d["nx"]) / n
        gfps = 1000.0 * d["gf"] / d["gint"]
        old = sum(x[2] * 10 / x[0] for x in d["nx"]) / n
        print(f"{name[:36]:36s} {avg(0):7.1f} {gfps:9.1f} {avg(2):6.0f} {avg(2)*10/gfps:17.1f} {old:15.1f} {d['draw']/max(1,d['rn']):13.1f} {avg(3):8.0f} {d['dur']:7.1f}")
