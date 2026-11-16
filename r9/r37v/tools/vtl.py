#!/usr/bin/env python3
"""vtl.py <vtl.bin> [crudo N0 N1 | resumen] [--desde s] [--hasta s]
Linea de tiempo de NWII_V_TL (r37v): registros <q t_ns, I kind, I arg>."""
import struct, sys, collections
K = {1: "VI", 2: "INT", 3: "OCIO+", 4: "OCIO-", 5: "XFB", 6: "W_LISTO", 7: "R_TOMA", 8: "R_FIN", 9: "SWAP",
     10: "FIFO_ESPERA", 12: "BP", 13: "S2_ESPERA", 14: "VSPIN", 15: "ATRASO"}
def load(p):
    raw = open(p, "rb").read()
    ev = [e for e in struct.iter_unpack("<qII", raw[:len(raw) // 16 * 16]) if e[1] in K]
    ev.sort(key=lambda e: e[0])
    return ev
def opt(n, d=None):
    if n in sys.argv:
        i = sys.argv.index(n); v = sys.argv[i + 1]; del sys.argv[i:i + 2]; return v
    return d
def main():
    desde = float(opt("--desde", 0)); hasta = float(opt("--hasta", 1e9))
    ev = load(sys.argv[1]); cmd = sys.argv[2] if len(sys.argv) > 2 else "resumen"
    t0 = ev[0][0]
    ev = [e for e in ev if desde <= (e[0] - t0) / 1e9 < hasta]
    if cmd == "crudo":
        a, b = int(sys.argv[3]), int(sys.argv[4])
        last = None
        for t, k, arg in ev[a:b]:
            d = "" if last is None else "+%.2f" % ((t - last) / 1e6)
            s = K[k]
            if k == 1: s += " lanzado" if arg & 1 else " NO(%s)" % ",".join(n for b_, n in ((2, "mascara"), (4, "callback"), (8, "pendiente"), (16, "despacho")) if arg & b_)
            elif k == 2: s += " %d%s" % (arg & 0xff, " (sonido)" if arg & 0x100 else "")
            elif k == 12: s += " %02x=%04x" % (arg >> 24, arg & 0xffff)
            else: s += " %d" % arg
            print("%12.3f %8s  %s" % ((t - t0) / 1e6, d, s))
            last = t
        return
    # resumen por fotograma del juego (XFB)
    vis = sorted([e[0] for e in ev if e[1] == 1 and e[2] & 1 and not e[2] & 64] + [e[0] for e in ev if e[1] == 15])
    vino = [e for e in ev if e[1] == 1 and not e[2] & 1]
    xfb = [e[0] for e in ev if e[1] == 5]
    import bisect
    print("VI lanzados %d, VI NO lanzados %d %s" % (len(vis), len(vino), collections.Counter(e[2] for e in vino).most_common(5)))
    # VI -> siguiente XFB
    lat = []
    for x in xfb:
        i = bisect.bisect_right(vis, x) - 1
        if i >= 0: lat.append((x - vis[i]) / 1e6)
    lat.sort()
    if lat:
        print("retrazo -> copia XFB (ms): p10 %.2f mediana %.2f p90 %.2f max %.2f (n=%d)" % (lat[len(lat)//10], lat[len(lat)//2], lat[len(lat)*9//10], lat[-1], len(lat)))
    d = sorted((xfb[i+1] - xfb[i]) / 1e6 for i in range(len(xfb) - 1))
    if len(xfb) > 1:
        print("fotogramas del juego/s: %.2f" % ((len(xfb) - 1) / ((xfb[-1] - xfb[0]) / 1e9)))
    held = [e for e in ev if e[1] == 15]
    if held:
        hu = sorted((e[2] & 0xfffffff) / 1000.0 for e in held)
        print("retrazos retenidos (NWII_V_LATE): %d (al entrar en ocioso %d, por plazo %d), retencion mediana %.2f ms p90 %.2f max %.2f" % (
            len(held), sum(1 for e in held if e[2] >> 28 == 1), sum(1 for e in held if e[2] >> 28 == 2), hu[len(hu)//2], hu[len(hu)*9//10], hu[-1]))
    rd = [e[0] for e in ev if e[1] == 8]
    if len(rd) > 1:
        print("render: fotogramas dibujados/s %.2f" % ((len(rd) - 1) / ((rd[-1] - rd[0]) / 1e9)))
    if d:
        print("intervalo entre copias XFB (ms): mediana %.1f; > 25 ms: %.1f %%" % (d[len(d)//2], 100.0 * sum(1 for v in d if v > 25) / len(d)))
    tuberia(ev)
def tuberia(ev):
    import bisect
    def pct(v):
        v = sorted(v)
        return "mediana %5.2f p90 %5.2f" % (v[len(v)//2], v[len(v)*9//10]) if v else "-"
    xfb = [e[0] for e in ev if e[1] == 5]
    vis = [e[0] for e in ev if e[1] == 1 and e[2] & 1 and not e[2] & 64] + [e[0] for e in ev if e[1] == 15]
    vis.sort()
    ob = [e[0] for e in ev if e[1] == 3]   # empieza ocioso
    oe = [e[0] for e in ev if e[1] == 4]   # acaba ocioso
    ready = [(e[0], e[2]) for e in ev if e[1] == 6]
    take = {}; done = {}
    for e in ev:
        if e[1] == 7: take[e[2]] = e[0]
        if e[1] == 8: done[e[2]] = e[0]
    swaps = [e[0] for e in ev if e[1] == 9]
    # hilo del juego, por fotograma (entre copias XFB): trabajo, espera hasta el retrazo, retrazo -> XFB
    work, wait, v2x, nvi = [], [], [], collections.Counter()
    for i in range(len(xfb) - 1):
        a, b = xfb[i], xfb[i + 1]
        j = bisect.bisect_right(vis, b) - 1
        if j < 0 or vis[j] < a: continue
        v = vis[j]
        k = bisect.bisect_right(ob, v) - 1          # ultimo tramo ocioso que empieza antes del retrazo
        end = ob[k] if k >= 0 and ob[k] > a else v
        work.append((end - a) / 1e6); wait.append((v - end) / 1e6); v2x.append((b - v) / 1e6)
        nvi[bisect.bisect_right(vis, b) - bisect.bisect_right(vis, a)] += 1
    print("HILO DEL JUEGO por fotograma (n=%d): trabajo hasta empezar a esperar %s | espera hasta el retrazo %s | retrazo -> copia XFB %s" % (
        len(work), pct(work), pct(wait), pct(v2x)))
    print("  retrazos por fotograma: " + " ".join("%d:%.1f%%" % (k, 100.0 * c / max(1, sum(nvi.values()))) for k, c in sorted(nvi.items())))
    # tuberia por fotograma dibujado
    lat_w, lat_t, dr, lat_s, tot = [], [], [], [], []
    merged = 0
    used = set()
    for t, sq in ready:
        i = bisect.bisect_right(xfb, t) - 1
        if i < 0: continue
        x = xfb[i]
        if sq in take and sq in done and take[sq] >= t:
            lat_w.append((t - x) / 1e6); lat_t.append((take[sq] - t) / 1e6); dr.append((done[sq] - take[sq]) / 1e6)
            k = bisect.bisect_left(swaps, done[sq])
            if k < len(swaps):
                lat_s.append((swaps[k] - done[sq]) / 1e6); tot.append((swaps[k] - x) / 1e6)
        used.add(i)
    print("TUBERIA por fotograma dibujado (n=%d): XFB -> trabajador listo %s | listo -> render lo toma %s | render %s | fin render -> swap hecho %s | XFB -> swap (latencia) %s" % (
        len(dr), pct(lat_w), pct(lat_t), pct(dr), pct(lat_s), pct(tot)))
    nr = len(ready); ntk = len([1 for t, sq in ready if sq in take])
    print("  fotogramas del juego %d, entregados por el trabajador %d, dibujados %d (no dibujados %d: el trabajador los junto con el siguiente o el render no los tomo)" % (
        len(xfb), nr, ntk, len(xfb) - ntk))

main()
