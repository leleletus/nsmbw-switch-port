#!/usr/bin/env python3
"""por_escena.py <bench_dir> [<bench_dir> ...]: per-scene stats of forced-demo runs (front S playlists).

Cuts the 5-s "[NX] game" lines of nwii.log by the "[DEMO] start <id>" / "[DEMO] end <id>" lines (they are
interleaved in time order) and prints, per scene and per run: mean / min fps and the mean busy % of the render
thread (main), game thread (cpu) and core-2 worker (gxw), plus the game-thread work per frame in ms
(cpu% x frame time): at stock clocks most scenes sit at a locked 30 fps, so fps hides gains; the ms column does not. The first 5-s window of each scene is skipped (it
contains the load). Several dirs → one table per scene, one column block per run, for A/B comparisons.
"""
import re
import sys
from collections import defaultdict

NX = re.compile(r"game ([0-9.]+) fps.*threads main ([0-9]+)%, cpu ([0-9]+)%.*gxw ([0-9]+)%")


def per_scene(path):
    scenes = defaultdict(list)          # id -> list of (fps, main, cpu, gxw)
    cur, skip = None, 0
    for line in open(path, errors="replace"):
        if line.startswith("[DEMO] start "):
            cur, skip = line.split()[2], 1
        elif line.startswith("[DEMO] end "):
            cur = None
        elif cur and line.startswith("[NX] game"):
            m = NX.search(line)
            if not m:
                continue
            if skip:
                skip -= 1
                continue
            scenes[cur].append(tuple(float(x) for x in m.groups()))
    return scenes


def main():
    runs = [(d.rstrip("/").split("/")[-1], per_scene(d.rstrip("/") + "/nwii.log")) for d in sys.argv[1:]]
    ids = []
    for _, sc in runs:
        for k in sc:
            if k not in ids:
                ids.append(k)
    for sid in ids:
        print(f"\n== {sid}")
        print(f"{'corrida':40s} {'n':>3s} {'fps med':>8s} {'fps min':>8s} {'render%':>8s} {'juego%':>7s} {'nucleo2%':>9s} {'ms juego/fot':>12s}")
        for name, sc in runs:
            v = sc.get(sid, [])
            if not v:
                print(f"{name[:40]:40s}   -")
                continue
            n = len(v)
            avg = lambda i: sum(x[i] for x in v) / n
            print(f"{name[:40]:40s} {n:3d} {avg(0):8.1f} {min(x[0] for x in v):8.1f} {avg(1):8.0f} {avg(2):7.0f} {avg(3):9.0f} {sum(x[2] * 10 / x[0] for x in v) / n:12.1f}")


if __name__ == "__main__":
    main()
