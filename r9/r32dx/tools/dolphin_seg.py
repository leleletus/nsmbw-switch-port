#!/usr/bin/env python3
"""dolphin_seg.py <serie.tsv>: ciclos ocupados del Broadway por fotograma en Dolphin, por escena (ventana 'main').

Mismas ventanas que dx.cpp: demos exef 120..2300, título exef 120..840, cinemática desde 3 s después de empezar.
ocupado = Δglobal_timer − Δidled (ciclos de 729 MHz); fotograma = retrazado de VI (el original va a 60 fps).
"""
import collections
import sys


def dolphin_busy(path, orden="desierto4j,agua_pinguino,cielo_paratroopas,nubes_plataformas"):
    """-> lista de (escena, fotogramas VI, ciclos/fot, ocupados/fot, exef/fot, velocidad)."""
    rows = []
    for ln in open(path):
        p = ln.split("\t")
        if p[0] == "wall":
            continue
        rows.append((float(p[0]),) + tuple(int(x) for x in p[1:]))
    orden = orden.split(",")
    segs, cur = [], None
    for r in rows:
        key = (r[4], r[5])
        if cur is None or cur[0] != key:
            cur = [key, []]
            segs.append(cur)
        cur[1].append(r)
    k, out = 0, []
    for key, rs in segs:
        scene, gmode = key
        if scene == 5 and gmode == 3:
            name = orden[k % len(orden)]
            k += 1
            w = [r for r in rs if 120 <= r[6] < 2300]
        elif scene == 5 and gmode == 2:
            name = "titulo"
            w = [r for r in rs if 120 <= r[6] < 840]
        elif scene == 8:
            name = "castillo"
            w = [r for r in rs if r[3] >= rs[0][3] + 180]
        else:
            continue
        if len(w) < 10:
            continue
        a, b = w[0], w[-1]
        fr = b[3] - a[3]
        dt, di = b[1] - a[1], b[2] - a[2]
        if fr > 100:
            out.append((name, fr, dt / fr, (dt - di) / fr, (b[6] - a[6]) / fr, fr / 60.0 / (b[0] - a[0])))
    return out


def dolphin_mean(path):
    acc = collections.defaultdict(lambda: [0.0, 0])
    for name, fr, _c, busy, _e, _v in dolphin_busy(path):
        acc[name][0] += busy * fr
        acc[name][1] += fr
    return {k: v[0] / v[1] for k, v in acc.items()}


if __name__ == "__main__":
    print(f"{'escena':20s} {'fotog':>6s} {'ciclos/fot':>11s} {'ocupados/fot':>13s} {'% ocupado':>9s} {'exef/fot':>8s} {'x real':>6s}")
    for name, fr, c, busy, e, v in dolphin_busy(sys.argv[1], *(sys.argv[2:3])):
        print(f"{name:20s} {fr:6d} {c:11.0f} {busy:13.0f} {100.0 * busy / c:9.1f} {e:8.3f} {v:6.2f}")
