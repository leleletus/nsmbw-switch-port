#!/usr/bin/env python3
"""[r45p] pantalla.py <bench_dir|run.log>... [--sel N] [--skip W]: diagnóstico por hilo de las corridas SIN demos
(mapa del mundo / selector de mundos «−»), troceadas por la entrada guionizada.

Tramos: «menus» hasta «[SCENE] … phase 1 starts» (llegada al mapa); «mapa» desde ahí; con --sel N (fotogramas del
juego tras la fase 1 en que el guion pulsa «−»; P02/P03 usan 300) «selector» desde N + 120 fotogramas. Se saltan las
W primeras ventanas de cada tramo (--skip, 1 por defecto: carga/transición).
Por tramo (medias de ventana):
  dib       fps dibujados ([NX] game X fps: fotogramas que el render presentó)
  juego     fps del juego ([PIPE] game frames / interval) ; msJ = ms de trabajo del hilo del juego por fotograma del
            juego = cpu% x 10 / fps juego (como r31t/por_escena_real.py)
  rend      [PIPE] draw avg (ms que tarda el render en dibujar un fotograma) ; loop = vuelta del bucle de render
  swap      [PIPE] swap avg/max (presentación: aquí se ve la espera a la GPU si la GPU no da abasto)
  wrk       [WPROF] busy_us/frame (trabajador gxw: parse+decode por fotograma preparado) ; w→r: worker frame-end->ready
  gx/gl     draws GX y draws GL por fotograma ([RPROF]) ; drop: fotogramas descartados por ventana
  efbc      copias EFB por fotograma ([TCSTAT], cada 600 fotogramas) ; hash: texturas hasheadas por fotograma
  hilos     [NX] threads main/cpu/gxw/snd %
"""
import re, sys, os
from collections import defaultdict

NX = re.compile(r"\[NX\] game ([0-9.]+) fps.*cpu (\d+) MHz, gpu (\d+) MHz.*busy max (\d+) ms.*threads main (\d+)%, cpu (\d+)%.*gxw (\d+)%, snd (\d+)%")
PIPE = re.compile(r"\[PIPE\] game frames=(\d+) interval avg=([0-9.]+) max=([0-9.]+).*worker frame-end->ready avg=([0-9.]+) max=([0-9.]+) ms \(n=(\d+)\) \| render ready->taken avg=([0-9.]+) max=[0-9.]+, draw avg=([0-9.]+) max=([0-9.]+) ms \(n=(\d+)\) \| swap avg=([0-9.]+) max=([0-9.]+) ms, loop avg=([0-9.]+) max=([0-9.]+)")
RP = re.compile(r"\[RPROF\] frames=(\d+) render_us/frame=(\d+).*gx_draws/frame=(\d+) gl_draws/frame=(\d+).*dropped_frames=(\d+)")
WP = re.compile(r"\[WPROF\] frames=(\d+) calls=(\d+) busy_us/frame=(\d+) .*parse=(\d+) .*decode=(\d+) .*cmds/frame=(\d+)")
TC = re.compile(r"\[TCSTAT\] per frame: hash_ram tex=([0-9.]+) .*efb copies=([0-9.]+)")
GX = re.compile(r"\[GPUXF\] per frame: draws gpu=(\d+) \(display-list (\d+).*gl=(\d+)")

def scan(path, sel):
    seg = "menus"; gf = 0
    S = defaultdict(lambda: defaultdict(list))
    for l in open(path, errors="replace"):
        if "phase 1 starts" in l:
            seg = "mapa"; gf = 0; continue
        if l.startswith("[PIPE]"):
            m = PIPE.search(l)
            if not m: continue
            if seg != "menus":
                gf += int(m.group(1))
                if sel and seg == "mapa" and gf > sel + 120:
                    seg = "selector"; continue   # ventana de transición fuera
            g = list(map(float, m.groups()))
            S[seg]["pipe"].append(g)
        elif l.startswith("[NX] game"):
            m = NX.search(l)
            if m: S[seg]["nx"].append(list(map(float, m.groups())))
        elif l.startswith("[RPROF]"):
            m = RP.search(l)
            if m: S[seg]["rp"].append(list(map(float, m.groups())))
        elif l.startswith("[WPROF]"):
            m = WP.search(l)
            if m: S[seg]["wp"].append(list(map(float, m.groups())))
        elif l.startswith("[TCSTAT]"):
            m = TC.search(l)
            if m: S[seg]["tc"].append(list(map(float, m.groups())))
        elif l.startswith("[GPUXF] per frame"):
            m = GX.search(l)
            if m: S[seg]["gx"].append(list(map(float, m.groups())))
    return S

def main():
    a = sys.argv[1:]
    sel = 0; skip = 1; paths = []
    i = 0
    while i < len(a):
        if a[i] == "--sel": sel = int(a[i + 1]); i += 2; continue
        if a[i] == "--skip": skip = int(a[i + 1]); i += 2; continue
        paths.append(a[i]); i += 1
    hdr = f"{'corrida':30s} {'tramo':8s} {'n':>3s} {'dib':>5s} {'juego':>5s} {'msJ':>5s} {'rend':>5s} {'loop':>5s} {'swap':>9s} {'wrk':>5s} {'w→r':>5s} {'gx':>6s} {'gl':>4s} {'drop':>4s} {'efbc':>5s} {'hash':>5s}  hilos m/c/g/s  MHz"
    print(hdr)
    for p in paths:
        L = p if os.path.isfile(p) else os.path.join(p, "nwii.log")
        if not os.path.isfile(L): L = os.path.join(p, "run.log")
        name = os.path.basename(p.rstrip("/")) if os.path.isdir(p) else os.path.basename(os.path.dirname(p))
        S = scan(L, sel)
        for seg in ("menus", "mapa", "selector"):
            d = S.get(seg)
            if not d: continue
            k = lambda key: d[key][skip:] if len(d[key]) > skip else d[key]
            nx, pi, rp, wp, tc = k("nx"), k("pipe"), k("rp"), k("wp"), d["tc"]
            av = lambda rows, j: sum(r[j] for r in rows) / len(rows) if rows else float("nan")
            gfr = sum(r[0] for r in pi); gint = sum(r[0] * r[1] for r in pi)
            gfps = 1000.0 * gfr / gint if gint else float("nan")
            msj = av(nx, 5) * 10 / gfps if nx and gfps == gfps else float("nan")
            mhz = sorted(set(f"{int(r[1])}/{int(r[2])}" for r in nx))
            print(f"{name[:30]:30s} {seg:8s} {len(pi):3d} {av(nx,0):5.1f} {gfps:5.1f} {msj:5.1f} {av(pi,7):5.1f} {av(pi,12):5.1f} "
                  f"{av(pi,10):4.1f}/{max([r[11] for r in pi] or [0]):<4.0f} {av(wp,2)/1000:5.1f} {av(pi,3):5.1f} {av(rp,2):6.0f} {av(rp,3):4.0f} "
                  f"{av(rp,4):4.1f} {av(tc,1):5.1f} {av(tc,0):5.0f}  {av(nx,4):.0f}/{av(nx,5):.0f}/{av(nx,6):.0f}/{av(nx,7):.0f}  {' '.join(mhz)}")

main()
