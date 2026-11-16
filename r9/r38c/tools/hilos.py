#!/usr/bin/env python3
"""hilos.py <bench_dir>...: por escena de la lista fija (y la fiesta del castillo aparte), medias de las
ventanas de 5 s del registro: % de CPU por hilo (ticks del núcleo: main, cpu, aud, gxw, snd), línea [S2]
(AID/s, irq y hilo ms/s, espera S, peticiones de mutex ms/s, retraso máx.), listas AX/s, kHz de salida,
underruns (incremento) y ventanas con AID < 320/s. La fiesta = ventanas cuya última [RPROF] tiene
gx_draws/frame >= 11500. Se saltan las 2 primeras ventanas de 5 s de cada demo (carga)."""
import re, sys
NX = re.compile(r"\[NX\] game ([0-9.]+) fps.*?cpu (\d+) MHz, gpu (\d+) MHz.*?ax thread (\d+) lists/s mix ([0-9.]+) us")
TH = re.compile(r"threads ([^%]+%(?:, [a-z]+ \d+%)*)")
OUT = re.compile(r"out ([0-9.]+) kHz, underruns (\d+)")
S2 = re.compile(r"\[S2\] [^:]+: AID (\d+)/s.*?irq ([0-9.]+) ms/s, hilo ([0-9.]+) \+ ([0-9.]+) ms/s, espera S ([0-9.]+) ms/s, "
                r"retraso max (\d+) us \| mutex (\d+)/s.*?sin juego (\d+)/s, ([0-9.]+) ms/s\).*?juego: S secc (\d+)/s irq (\d+)/s, espera ([0-9.]+) ms/s")
C2 = re.compile(r"\[C2\] (.*)")
RP = re.compile(r"\[RPROF\].*gx_draws/frame=(\d+)")

def run(d):
    rows = {}
    scene, skip, last_gx, pend = None, 0, 0, None
    last_und = None
    order = []
    for l in open(d.rstrip("/") + "/nwii.log", errors="replace"):
        if l.startswith("[DEMO] start "):
            scene = l.split()[2]; skip = 2; continue
        if l.startswith("[DEMO] end "):
            scene = None; continue
        m = RP.search(l)
        if m: last_gx = int(m.group(1)); continue
        m = NX.search(l)
        if m:
            o = OUT.search(l)
            und = int(o.group(2)) if o else None
            dund = (und - last_und) if (und is not None and last_und is not None) else 0
            last_und = und
            if scene is None: pend = None; continue
            if skip: skip -= 1; pend = None; continue
            th = {}
            t = TH.search(l)
            if t:
                for part in t.group(1).split(", "):
                    k, v = part.rsplit(" ", 1); th[k.strip()] = float(v.rstrip("%"))
            key = scene + ("/fiesta" if scene == "castillo" and last_gx >= 11500 else "")
            pend = dict(key=key, fps=float(m.group(1)), mhz=(int(m.group(2)), int(m.group(3))), ax=int(m.group(4)),
                        mix=float(m.group(5)), th=th, khz=float(o.group(1)) if o else 0, dund=dund)
            rec = pend
            rows.setdefault(key, []).append(rec)
            if key not in order: order.append(key)
            continue
        m = S2.search(l)
        if m and pend is not None:
            g = m.groups()
            pend.update(aid=int(g[0]), irq=float(g[1]), hilo=float(g[2]) + float(g[3]), espS=float(g[4]), late=int(g[5]) / 1000,
                        mtx=float(g[8]), gsec=int(g[9]), gesp=float(g[11]))
            continue
        m = C2.search(l)
        if m and pend is not None:
            pend.setdefault("c2", []).append(m.group(1))
    return order, rows

def avg(rs, k, sub=None):
    v = [(r[k] if sub is None else r[k].get(sub)) for r in rs if k in r and (sub is None or sub in r[k])]
    v = [x for x in v if x is not None]
    return sum(v) / len(v) if v else float("nan")

for d in sys.argv[1:]:
    order, rows = run(d)
    print(d.rstrip("/").split("/")[-1])
    print(f"  {'escena':26s} vent  fps | main  cpu  gxw  snd  aud | AID  bajas | irq  hilo espS late | mtx  gesp | AX/s kHz  und+")
    for k in order:
        rs = rows[k]
        low = sum(1 for r in rs if r.get("aid", 999) < 320)
        mhz = {r["mhz"] for r in rs}
        print(f"  {k:26s} {len(rs):4d} {avg(rs,'fps'):4.1f} | {avg(rs,'th','main'):4.0f} {avg(rs,'th','cpu'):4.0f} {avg(rs,'th','gxw'):4.0f} "
              f"{avg(rs,'th','snd'):4.0f} {avg(rs,'th','aud'):4.0f} | {avg(rs,'aid'):5.1f} {low:3d} | {avg(rs,'irq'):5.1f} {avg(rs,'hilo'):5.1f} "
              f"{avg(rs,'espS'):4.2f} {max([r.get('late',0) for r in rs]):5.1f} | {avg(rs,'mtx'):5.1f} {avg(rs,'gesp'):5.2f} | {avg(rs,'ax'):4.0f} {avg(rs,'khz'):4.1f} {sum(r['dund'] for r in rs):4d}"
              + ("" if mhz == {(1020, 460)} else f"  RELOJ {sorted(mhz)}"))
