#!/usr/bin/env python3
"""escena_a.py <bench_dir>: por escena (saltando 2 ventanas tras cada [DEMO] start): mezcla AX us/lista, [S2] irq/hilo,
[A43] (esperas cv, cesiones, siestas cortas, sondeos, irq DSP, peticiones, giro/pet, siestas pet); al final la media
acumulada de la reverb (kernel 2 o 4, us/llamada)."""
import re, sys, collections
d = sys.argv[1].rstrip("/")
NX = re.compile(r"\[NX\] game .*?ax thread (\d+) lists/s mix ([0-9.]+) us")
S2 = re.compile(r"\[S2\] [^:]+: AID (\d+)/s.*?irq ([0-9.]+) ms/s, hilo ([0-9.]+) \+ ([0-9.]+) ms/s")
A = re.compile(r"\[A43\] sonido: esperas cv (\d+)/s, cesiones (\d+)/s, siestas cortas (\d+)/s \| sondeos (\d+)/s \| irq DSP ([0-9.]+) ms/s \(AID ([0-9.]+)\) \| peticiones (\d+)/s, giro ([0-9.]+) vueltas/pet, siestas (\d+)/s \| S tomado (\d+)/s, ganchos SO (\d+)/s")
acc = collections.defaultdict(lambda: collections.defaultdict(list)); order = []
scene = None; skip = 0; k2 = k4 = None
for l in open(d + "/nwii.log", errors="replace"):
    if l.startswith("[DEMO] start "):
        scene = l.split()[2]; skip = 2
        if scene not in order: order.append(scene)
        continue
    m = re.search(r"kernel2 ([0-9.]+) us/call \((\d+)\)", l)
    if m and int(m.group(2)): k2 = float(m.group(1))
    m = re.search(r"\[A43\] axfx kernel4 ([0-9.]+) us/call", l)
    if m: k4 = float(m.group(1))
    if scene is None: continue
    m = NX.search(l)
    if m:
        if skip: skip -= 1; continue
        acc[scene]["mix"].append(float(m.group(2)))
    m = S2.search(l)
    if m and not skip:
        acc[scene]["irq"].append(float(m.group(2))); acc[scene]["hilo"].append(float(m.group(3)) + float(m.group(4)))
    m = A.search(l)
    if m and not skip:
        for i, k in enumerate(["cv", "ces", "sies", "sond", "dsp", "aid", "pet", "giro", "spet", "S", "os"]):
            acc[scene][k].append(float(m.group(i + 1)))
f = lambda v: "%.0f" % (sum(v) / len(v)) if v else "-"
g = lambda v: "%.1f" % (sum(v) / len(v)) if v else "-"
print("%-18s %6s %6s %6s | %5s %5s %5s %6s %5s %5s | %5s %5s %5s" % ("escena", "mix us", "irq", "hilo", "cv/s", "ces/s", "sie/s", "sond/s", "dsp", "aid", "pet/s", "giro", "spet"))
for s in order:
    a = acc[s]
    print("%-18s %6s %6s %6s | %5s %5s %5s %6s %5s %5s | %5s %5s %5s" % (s, g(a["mix"]), g(a["irq"]), g(a["hilo"]), f(a["cv"]), f(a["ces"]),
          f(a["sies"]), f(a["sond"]), g(a["dsp"]), g(a["aid"]), f(a["pet"]), g(a["giro"]), f(a["spet"])))
print("reverb: kernel2 %s us/llamada, kernel4 %s us/llamada (medias acumuladas)" % (k2, k4))
