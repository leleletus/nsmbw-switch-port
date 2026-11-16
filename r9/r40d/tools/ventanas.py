#!/usr/bin/env python3
"""ventanas.py <bench_dir> [demo] [--sum]: por ventana de 2 s (lineas [RPROF]/[PIPE]/[WPROF] del hilo de render) de cada
demo: fot. juego, dibujados, descartados (dropped) = retirados (retracts) + descartes por lote con dos finales
(drop2 = dropped - retracts), render ms/fot, espera listo->tomado, trabajador: fot preparados, llamadas, busy ms/fot,
parse, split, decode ms/fot; [D40] si existe. --sum: solo medias por demo (sin las 2 primeras ventanas)."""
import re, sys
RP = re.compile(r"\[RPROF\] frames=(\d+) render_us/frame=(\d+).*gx_draws/frame=(\d+).*dropped_frames=(\d+).*retracts=(\d+)")
PI = re.compile(r"\[PIPE\] game frames=(\d+) .*worker frame-end->ready avg=([0-9.]+) max=([0-9.]+) ms \(n=(\d+)\) \| render ready->taken avg=([0-9.]+) max=[0-9.]+, draw avg=([0-9.]+)")
WP = re.compile(r"\[WPROF\] frames=(\d+) calls=(\d+) busy_us/frame=(\d+) .*parse=(\d+) scan=\d+ clear=(\d+) split=(\d+) decode=(\d+)")
D4 = re.compile(r"\[D40\] (.*)")
args = [a for a in sys.argv[1:] if not a.startswith("--")]
summ = "--sum" in sys.argv
d = args[0].rstrip("/"); want = args[1] if len(args) > 1 else None
demo = None; rows = {}; cur = {}
def flush():
    if demo and "rp" in cur and "pi" in cur:
        rows.setdefault(demo, []).append(dict(cur))
import os
log = d + "/nwii.log" if os.path.exists(d + "/nwii.log") else d + "/run.log"
for l in open(log, errors="replace"):
    if l.startswith("[DEMO] start "):
        flush(); cur = {}; demo = l.split()[2]; continue
    if l.startswith("[DEMO] end "):
        flush(); cur = {}; demo = None; continue
    m = RP.search(l)
    if m:
        flush(); cur = {"rp": [int(x) for x in m.groups()]}; continue
    m = PI.search(l)
    if m and "rp" in cur: cur["pi"] = [float(x) for x in m.groups()]; continue
    m = WP.search(l)
    if m and "rp" in cur: cur["wp"] = [int(x) for x in m.groups()]; continue
    m = D4.search(l)
    if m and "rp" in cur: cur["d4"] = m.group(1).strip()
flush()
hdr = "%-5s %5s %5s %5s %5s %5s | %6s %6s %6s | %5s %5s %6s %6s %6s %6s" % (
    "win", "juego", "dib", "drop", "retr", "drop2", "rendms", "wait", "w_lat", "wfot", "calls", "busy", "parse", "split", "decod")
for dm, rs in rows.items():
    if want and dm != want: continue
    if not summ:
        print("== %s" % dm); print(hdr)
    acc = []
    for i, r in enumerate(rs):
        rp, pi, wp = r["rp"], r["pi"], r.get("wp", [0] * 7)
        v = [pi[0] / 2, rp[0] / 2, rp[3] / 2, rp[4] / 2, (rp[3] - rp[4]) / 2, pi[5], pi[4], pi[1], wp[0] / 2, wp[1] / 2,
             wp[2] / 1000, wp[3] / 1000, wp[5] / 1000, wp[6] / 1000]
        if i >= 2: acc.append(v)
        if not summ:
            print("%-5d %5.1f %5.1f %5.1f %5.1f %5.1f | %6.1f %6.1f %6.1f | %5.1f %5.0f %6.1f %6.1f %6.1f %6.1f" % tuple([i] + v)
                  + ("  " + r["d4"] if "d4" in r else ""))
    if acc:
        n = len(acc); m = [sum(x[k] for x in acc) / n for k in range(len(acc[0]))]
        print(("%-18s n=%2d juego %5.1f dib %5.1f drop %5.1f retr %5.1f drop2 %5.1f | render %5.1f wait %5.1f w_lat %5.1f"
               " | wfot %5.1f calls %4.0f busy %5.1f parse %5.1f split %5.1f decode %5.1f  min_dib %5.1f") % tuple([dm, n] + m + [min(x[1] for x in acc)]))
