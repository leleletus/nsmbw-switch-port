#!/usr/bin/env python3
"""fiesta.py <bench_dir>...: the castle demo split in two by the draw count of each 2 s window ([RPROF]
gx_draws/frame >= 11500: the party, 12 200 draws a frame; the rest: 6 600-11 000). Per part: [PIPE] game
frames/s, drawn frames/s ([RPROF] frames), render draw avg (ms, as por_escena_real.py), loop avg, the worker's
busy us per prepared frame ([WPROF]) and its split/parse, dropped frames per window. The first two windows of
the demo (load) are skipped. Without NWII_R_CULL the gx_draws count is the same, so both runs split alike."""
import re, sys
RP = re.compile(r"\[RPROF\] frames=(\d+) render_us/frame=(\d+).*gx_draws/frame=(\d+).*dropped_frames=(\d+)")
PI = re.compile(r"\[PIPE\] game frames=(\d+) interval avg=([0-9.]+).*draw avg=([0-9.]+) max=[0-9.]+ ms \(n=(\d+)\).*loop avg=([0-9.]+)")
WP = re.compile(r"\[WPROF\] frames=(\d+) .*busy_us/frame=(\d+) .*parse=(\d+) .*split=(\d+) decode=(\d+)")
PH = re.compile(r"\[RPHASE\] us/frame: total=(\d+) regs=(\d+) state=(\d+) tex=(\d+) emit=(\d+) upload=(\d+) gldraw=(\d+) copy=(\d+) frame=(\d+)")
PHN = ["total", "regs", "state", "tex", "emit", "upload", "gldraw", "copy", "frame"]
for d in sys.argv[1:]:
    parts = {"fiesta": [], "resto": []}
    cur, skip, last_kind = False, 0, None
    rec = {}
    ph = {"fiesta": [], "resto": []}
    last_gx = None
    for l in open(d.rstrip("/") + "/nwii.log", errors="replace"):
        if l.startswith("[DEMO] start "):
            cur = l.split()[2] == "castillo"; skip = 2; rec = {}
            continue
        if l.startswith("[DEMO] end "):
            cur = False
            continue
        if not cur:
            continue
        m = PH.search(l)
        if m and last_gx is not None:
            ph["fiesta" if last_gx >= 11500 else "resto"].append([int(x) for x in m.groups()])
            continue
        m = RP.search(l)
        if m:
            last_gx = int(m.group(3))
            if skip:
                skip -= 1; rec = {}; continue
            rec = {"rframes": int(m.group(1)), "gx": int(m.group(3)), "drop": int(m.group(4))}
            continue
        m = PI.search(l)
        if m and "gx" in rec:
            rec.update(gf=int(m.group(1)), gint=float(m.group(2)), draw=float(m.group(3)), rn=int(m.group(4)),
                       loop=float(m.group(5)))
            continue
        m = WP.search(l)
        if m and "gf" in rec:
            rec.update(wf=int(m.group(1)), busy=int(m.group(2)), parse=int(m.group(3)), split=int(m.group(4)))
            parts["fiesta" if rec["gx"] >= 11500 else "resto"].append(rec)
            rec = {}
    name = d.rstrip("/").split("/")[-1]
    for k, rs in parts.items():
        if not rs:
            print(f"{name[:34]:34s} {k:6s} -"); continue
        gf = sum(r["gf"] for r in rs); gint = sum(r["gf"] * r["gint"] for r in rs)
        rn = sum(r["rn"] for r in rs); draw = sum(r["draw"] * r["rn"] for r in rs) / max(1, rn)
        loop = sum(r["loop"] * r["rn"] for r in rs) / max(1, rn)
        secs = gint / 1000.0
        wf = sum(r["wf"] for r in rs)
        busy = sum(r["busy"] * r["wf"] for r in rs) / max(1, wf)
        split = sum(r["split"] * r["wf"] for r in rs) / max(1, wf)
        parse = sum(r["parse"] * r["wf"] for r in rs) / max(1, wf)
        print(f"{name[:34]:34s} {k:6s} ventanas {len(rs):3d} | fps juego {1000*gf/gint:5.1f} dib {rn/secs:5.1f}"
              f" | render draw {draw:5.1f} ms loop {loop:5.1f} | trabajador {busy/1000:5.1f} ms/fot (parse {parse/1000:4.1f}"
              f" split {split/1000:4.1f}) | gx/fot {sum(r['gx'] for r in rs)/len(rs):6.0f}")
        if ph[k]:
            n = len(ph[k])
            print(" " * 42 + "RPHASE (%d): " % n + " ".join("%s=%.1f" % (PHN[i], sum(x[i] for x in ph[k]) / n / 1000.0) for i in range(len(PHN))))
