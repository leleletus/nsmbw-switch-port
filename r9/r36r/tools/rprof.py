#!/usr/bin/env python3
"""rprof.py <bench dir> <nm> [escena=castillo] [hilo=0|4] [N=40]: hilos de render (0) / trabajador (4) de una
escena con las muestras de tprof_host.bin (NWII_TPROF=1, runtime r31t+). Por muestra: la hoja y, si la hoja es
del driver/libc/nucleo, la primera funcion del runtime en la pila del anfitrion (quien llamo). Ademas, para las
funciones del runtime mas calientes, histograma de pc (para desensamblar)."""
import collections, os, struct, sys
sys.path.insert(0, "/home/djvemo/nsmbw-port/r9/r31t/tools")
import tprof  # noqa

d, nm = sys.argv[1], sys.argv[2]
esc = sys.argv[3] if len(sys.argv) > 3 else "castillo"
slot = int(sys.argv[4]) if len(sys.argv) > 4 else 0
N = int(sys.argv[5]) if len(sys.argv) > 5 else 40
pr = tprof.Prof(d, nm, None, None)
sid = tprof.DEMOS.index(esc) if esc in tprof.DEMOS else None
raw = open(os.path.join(d, "tprof_host.bin"), "rb").read()
h = list(struct.iter_unpack("<IIIIHbB", raw[:len(raw) // 20 * 20]))
# skip the first 5 s of each visit of the scene (load), like tprof.py
rs, start, prev = [], None, None
for r in h:
    if r[5] != prev:
        prev, start = r[5], r[0]
    if r[5] == sid and r[6] == slot and r[0] - start >= 5_000_000:
        rs.append(r)
n = len(rs)
print("%s hilo %d: %d muestras" % (esc, slot, n))
def isrt(s):
    return s and ("nwii" in s or s.startswith("_ZL") or "GXCommand" in s or "PrepFrame" in s or "RenderPacket" in s
                  or "ProcessGXFifo" in s)
leaf = collections.Counter(); caller = collections.Counter(); grp = collections.Counter(); pcs = collections.defaultdict(collections.Counter)
for r in rs:
    pc, lr, hs = r[1], r[2], r[3]
    s0 = pr.hsym(pc) or "?"
    chain = [pr.hsym(a) or "?" for a in (pr.stacks[hs][1:] if hs and hs < len(pr.stacks) else [lr])]
    j = " ".join([s0] + chain)
    if "GX_HostWait" in j: k = "ESPERA sin fotograma"
    elif "FenceWait" in j: k = "ESPERA GPU (fence)"
    elif "nwindow" in j or "Dequeue" in j: k = "ESPERA vsync/buffer"
    elif s0 in ("svcWaitProcessWideKeyAtomic", "svcArbitrateLock", "svcWaitSynchronization", "svcWaitForAddress", "svcSleepThread"):
        k = "ESPERA " + " < ".join(chain[:3])
    elif isrt(s0): k = "runtime"
    else: k = "driver/libc/nucleo"
    grp[k] += 1
    leaf[s0[:90]] += 1
    if isrt(s0):
        pcs[s0][pc] += 1
    elif not k.startswith("ESPERA"):
        c = next((x for x in chain if isrt(x)), "?")
        caller[(c[:70], s0[:40])] += 1
print("grupos: " + ", ".join("%s %.1f%%" % (k, 100.0 * v / n) for k, v in grp.most_common()))
print("-- hojas:")
for k, v in leaf.most_common(N):
    print("  %5.1f%%  %s" % (100.0 * v / n, k))
print("-- driver/libc por quien llama (runtime):")
cc = collections.Counter()
for (c, s), v in caller.items():
    cc[c] += v
for k, v in cc.most_common(15):
    print("  %5.1f%%  %s" % (100.0 * v / n, k))
print("-- (llamador, hoja):")
for (c, s), v in caller.most_common(25):
    print("  %5.1f%%  %s  <- %s" % (100.0 * v / n, s, c))
if len(sys.argv) > 6:
    f = sys.argv[6]
    base = next((a for a, sz, nmx in pr.hs if nmx == f), None)
    print("-- pc en %s (offset: muestras)" % f)
    for pc, v in sorted(pcs[f].items()):
        print("  +0x%x %d" % (pc - base, v))
