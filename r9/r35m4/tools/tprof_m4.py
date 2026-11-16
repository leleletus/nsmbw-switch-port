#!/usr/bin/env python3
"""tprof.py: analiza el perfil con pila invitada del frente T (NWII_TPROF=1, runtime de r9/r31t).

  tprof.py <dir con tprof_*.bin> <nm del binario (nm -n -S --defined-only)> <orden> [opciones]

Ordenes:
  resumen                   por escena: muestras, fotogramas, ms/fot ocupado, reparto hilo/ocioso
  top [N]                   funciones invitadas por tiempo INCLUSIVO (ms por fotograma), por escena
  arbol <dir hex> [prof] [min%]   arbol de llamadas bajo una funcion (todas las escenas o --escena)
  fases                     tabla de fases (reglas de FASES abajo) en ms/fot por escena
  esperas                   donde espera el hilo principal cuando no corre (pila 2) y esperas del anfitrion
  cruce                     tabla fase x subsistema (ms/fot)
  hilos [N]                 hilos de render y trabajador por escena: esperas (GPU, sin trabajo) y CPU
  linea                     linea de tiempo media del fotograma (fase dominante por tramo de 1 ms)
Opciones: --escena <nombre|indice>   --us <intervalo de muestreo en us, por defecto se deduce>
          --dol <main.dol> (por defecto game/SMNE01/sys/main.dol)   --syms <usa_symbols.txt>

Cadena de cada muestra (de fuera hacia dentro): funciones de las direcciones de retorno de la cadena de
marcos PPC + [sp+4] si es coherente (es un retorno tras un `bl` a la funcion siguiente, o tras una llamada
indirecta) + la funcion donde esta el pc del anfitrion (o, si el pc esta en el runtime, "rt:<simbolo>").
"""
import bisect
import collections
import os
import re
import struct
import sys

P = "/home/djvemo/nsmbw-port"
DEMOS = ["agua_pinguino", "pradera", "desierto4j", "desierto_helices", "nieve", "nubes_plataformas", "torre3",
         "playa_hielo", "playa_yoshi", "jungla", "cielo_hoja", "casa_oscura", "castillo6", "cielo_paratroopas",
         "lava", "castillo"]


def args_opt(name, default=None):
    if name in sys.argv:
        i = sys.argv.index(name)
        v = sys.argv[i + 1]
        del sys.argv[i:i + 2]
        return v
    return default


class Dol:
    def __init__(self, path):
        d = open(path, "rb").read()
        self.secs = []
        for i in range(18):
            off, addr, size = (struct.unpack_from(">I", d, b + 4 * i)[0] for b in (0, 0x48, 0x90))
            if size and i < 7:
                self.secs.append((addr, size, d[off:off + size]))

    def ins(self, a):
        for addr, size, data in self.secs:
            if addr <= a < addr + size - 3:
                return struct.unpack_from(">I", data, a - addr)[0]
        return None


class Prof:
    def __init__(self, d, nm, dol, symsf):
        self.dol = Dol(dol) if dol and os.path.exists(dol) else None
        # host symbols
        hs = []
        for l in open(nm):
            q = l.split()
            if len(q) == 4 and q[2] in "tTwW":
                hs.append((int(q[0], 16), int(q[1], 16), q[3]))
            elif len(q) == 3 and q[1] in "tTwW":
                hs.append((int(q[0], 16), 0, q[2]))
        hs.sort()
        self.hs, self.hst = hs, [s[0] for s in hs]
        g = set()
        self.gname = {}
        for a, s, n in hs:
            m = re.search(r"(?:^func_|_)(8[0-9A-Fa-f]{7})(?:_g6\w*)?$", n)
            if m:
                ga = int(m.group(1), 16)
                g.add(ga)
                if not n.startswith("func_"):
                    self.gname[ga] = n[:n.rfind("_8")]
        self.gst = sorted(g)
        if symsf and os.path.exists(symsf):
            for l in open(symsf):
                q = l.split()
                if len(q) >= 3:
                    try:
                        self.gname.setdefault(int(q[0], 16), q[2])
                    except ValueError:
                        pass
        self.gname.update(NOMBRES)
        # stacks
        raw = open(os.path.join(d, "tprof_stacks.bin"), "rb").read()
        w = struct.unpack("<%dI" % (len(raw) // 4), raw[:len(raw) // 4 * 4])
        self.stacks = [()]
        i = 0
        while i < len(w):
            n = w[i]
            if i + 1 + n > len(w):
                break
            self.stacks.append(w[i + 1:i + 1 + n])
            i += 1 + n
        raw = open(os.path.join(d, "tprof_samples.bin"), "rb").read()
        n = len(raw) // 32
        self.recs = list(struct.iter_unpack("<IIIIIIIHbB", raw[:n * 32]))
        self.cache = {}
        self.hcache = {}

    def hsym(self, o):
        i = bisect.bisect_right(self.hst, o) - 1
        if i >= 0 and o < self.hs[i][0] + max(4, self.hs[i][1]):
            return self.hs[i][2]
        return None

    def gfun(self, a):
        """guest address -> start of its function (or None)"""
        if not (0x80004000 <= a < 0x81800000):
            return None
        i = bisect.bisect_right(self.gst, a) - 1
        if i < 0 or a - self.gst[i] > 0x8000:
            return None
        return self.gst[i]

    def name(self, f):
        if isinstance(f, str):
            return f
        n = self.gname.get(f)
        return "%08X %s" % (f, n) if n else "%08X" % f

    def host_leaf(self, pc, lr):
        k = (pc, lr)
        r = self.hcache.get(k)
        if r is None:
            s = self.hsym(pc) if pc != 0xFFFFFFFF else None
            m = re.search(r"(?:^func_|_)(8[0-9A-Fa-f]{7})(?:_g6\w*)?$", s) if s else None
            if m:
                r = (int(m.group(1), 16), None)
            else:
                r = (None, "rt:" + (s or "?"))
            self.hcache[k] = r
        return r

    def chain_of(self, sid, leaf=None):
        """stack id -> tuple of guest function starts, outermost first (frame-0 hint resolved with `leaf`)"""
        k = (sid, leaf)
        c = self.cache.get(k)
        if c is not None:
            return c
        st = self.stacks[sid] if sid < len(self.stacks) else ()
        out = []
        if st and st[0] != 0xFFFFFFFF:
            for ra in reversed(st[3:]):
                f = self.gfun(ra)
                if f is not None:
                    out.append(f)
            # [sp+4]: only if coherent
            if len(st) > 2:
                ra = st[2]
                f = self.gfun(ra)
                if f is not None and (not out or out[-1] != f) and f != leaf:
                    ok = False
                    ins = self.dol.ins(ra - 4) if self.dol else None
                    if ins is not None:
                        if ins >> 26 == 18 and ins & 1:  # bl
                            off = ins & 0x03FFFFFC
                            if off & 0x02000000:
                                off -= 0x04000000
                            ok = leaf is not None and (ra - 4 + off) == leaf
                        elif ins in (0x4E800421, 0x4E800021):  # bctrl / blrl
                            ok = True
                    if ok:
                        out.append(f)
        c = tuple(out)
        self.cache[k] = c
        return c

    def sample_chain(self, r):
        """-> (kind, chain) kind: 'main' (hilo principal del juego), 'otro' (otro hilo invitado), 'ocio'"""
        t, pc, lr, s1, s2, cur, hs, frame, scene, flags = r
        gleaf, rt = self.host_leaf(pc, lr)
        ch = list(self.chain_of(s1, gleaf))
        if gleaf is not None:
            if not ch or ch[-1] != gleaf:
                ch.append(gleaf)
        else:
            # runtime helper: parent = generated function of the host lr, if any
            gl, _ = self.host_leaf(lr, 0)
            if gl is not None and (not ch or ch[-1] != gl):
                ch.append(gl)
            ch.append(rt)
        return ch


IDLE_RT = ("rt:svcSleepThread", "rt:_ZN4nwii7runtime9idle_waitERNS0_10CPUContextE", "rt:__clock_nanosleep",
           "rt:clock_nanosleep", "rt:nanosleep")
IDLE_FN = {0x801B4EA0}

# Nombres puestos a mano (USA) para funciones sin simbolo; se amplian segun se identifican.
NOMBRES = {
    0x801B4EA0: "SelectThread (bucle ocioso del SO)",
    0x801A2B10: "__AXSyncPBs?",
    0x80250E70: "g3d CalcWorld (nucleo)",
}

# Fases: (nombre, [funciones raiz]) en orden de prioridad; gana la raiz MAS INTERNA de la cadena que
# pertenezca a alguna fase (asi "sonido dentro de execute" cuenta como sonido si se quiere separar).
FASES = []
SUBSIS = []


def is_idle(pr, ch, r=None):
    """Ocioso: el pc invitado (ctx.pc) esta en el bucle ocioso del SO (SelectThread sin hilos listos) y el
    anfitrion no esta en otra funcion generada; o el anfitrion duerme (idle_wait / svcSleepThread)."""
    if not ch:
        return True
    last = ch[-1]
    if isinstance(last, str):
        if last in IDLE_RT or "idle_wait" in last or "nanosleep" in last or "SleepThread" in last:
            return True
    elif last not in IDLE_FN:
        return False
    if r is not None:
        st = pr.stacks[r[3]] if r[3] < len(pr.stacks) else ()
        if st and pr.gfun(st[0]) in IDLE_FN:
            return True
    return False


def scene_name(s):
    return DEMOS[s] if 0 <= s < len(DEMOS) else ("otra%d" % s if s != -1 else "sin_escena")


def main():
    us = args_opt("--us")
    esc = args_opt("--escena")
    dol = args_opt("--dol", P + "/game/SMNE01/sys/main.dol")
    symsf = args_opt("--syms", P + "/symbols/usa_symbols.txt")
    fasesf = args_opt("--fases", os.path.join(os.path.dirname(os.path.abspath(__file__)), "fases.py"))
    d, nm, cmd = sys.argv[1], sys.argv[2], sys.argv[3]
    rest = sys.argv[4:]
    if os.path.exists(fasesf):
        ns = {}
        exec(open(fasesf).read(), ns)
        NOMBRES.update(ns.get("NOMBRES", {}))
        FASES[:] = ns.get("FASES", [])
        SUBSIS[:] = ns.get("SUBSIS", [])
    pr = Prof(d, nm, dol, symsf)
    recs = pr.recs
    if not recs:
        print("sin muestras")
        return
    if us is None:
        dts = sorted(recs[i + 1][0] - recs[i][0] for i in range(min(len(recs) - 1, 20000)))
        us = dts[len(dts) // 2]
    us = float(us)
    # scenes: skip the first 5 s of each (load), like por_escena.py
    by_scene = collections.defaultdict(list)
    start = {}
    prev = None
    for r in recs:
        s = r[8]
        if s != prev:
            start[s] = r[0]
            prev = s
        if r[0] - start[s] < 5_000_000:
            continue
        by_scene[s].append(r)
    scenes = [s for s in by_scene if s >= 0] or list(by_scene)
    if esc is not None:
        scenes = [s for s in scenes if scene_name(s) == esc or str(s) == esc]

    def frames_of(rs):
        # fotogramas del juego vistos entre muestras seguidas (mismos huecos que wall_of)
        n = 0
        for i in range(1, len(rs)):
            dt = rs[i][0] - rs[i - 1][0]
            if 0 < dt < 50000:
                n += (rs[i][7] - rs[i - 1][7]) & 0xFFFF
        return max(n, 1)

    def wall_of(rs):
        # sum of gaps < 50 ms (the scene list loops: several visits)
        w = 0
        for i in range(1, len(rs)):
            dt = rs[i][0] - rs[i - 1][0]
            if 0 < dt < 50000:
                w += dt
        return w

    if cmd == "resumen":
        print("intervalo de muestreo %.0f us, %d muestras, %d pilas" % (us, len(recs), len(pr.stacks) - 1))
        print("%-20s %8s %7s %7s %9s %9s %9s %9s" % ("escena", "muestras", "fotogr", "ms/fot", "principal", "otro hilo", "ocioso", "ms ocup/fot"))
        for s in scenes:
            rs = by_scene[s]
            fr = frames_of(rs)
            wall = wall_of(rs) / 1000.0
            k = collections.Counter()
            for r in rs:
                ch = pr.sample_chain(r)
                if is_idle(pr, ch, r):
                    k["ocio"] += 1
                elif r[9] & 1:
                    k["main"] += 1
                else:
                    k["otro"] += 1
            n = len(rs)
            sc = wall / n if n else 0   # ms per sample (real)
            print("%-20s %8d %7d %7.2f %8.1f%% %8.1f%% %8.1f%% %9.2f" % (
                scene_name(s), n, fr, wall / fr, 100 * k["main"] / n, 100 * k["otro"] / n, 100 * k["ocio"] / n,
                (k["main"] + k["otro"]) * sc / fr))
        return

    def per_frame_ms(rs):
        fr = frames_of(rs)
        wall = wall_of(rs) / 1000.0
        return (wall / len(rs)) / fr   # ms per frame that one sample is worth

    if cmd == "top":
        N = int(rest[0]) if rest else 60
        for s in scenes:
            rs = by_scene[s]
            w = per_frame_ms(rs)
            inc = collections.Counter()
            slf = collections.Counter()
            for r in rs:
                ch = pr.sample_chain(r)
                if is_idle(pr, ch, r):
                    continue
                for f in set(ch):
                    inc[f] += 1
                slf[ch[-1]] += 1
            print("\n== %s: tiempo inclusivo (ms/fot), propio" % scene_name(s))
            for f, c in inc.most_common(N):
                print("  %7.3f %7.3f  %s" % (c * w, slf[f] * w, pr.name(f)))
        return

    if cmd == "arbol":
        root = int(rest[0], 16) if rest[0] not in ("raiz", "root") else None
        depth = int(rest[1]) if len(rest) > 1 else 4
        minp = float(rest[2]) if len(rest) > 2 else 2.0
        tree = {}
        tot = 0
        wsum = 0.0
        for s in scenes:
            rs = by_scene[s]
            w = per_frame_ms(rs) / len(scenes)
            for r in rs:
                ch = pr.sample_chain(r)
                if is_idle(pr, ch, r):
                    continue
                if root is not None:
                    if root not in ch:
                        continue
                    ch = ch[ch.index(root):]
                node = tree
                for f in ch[:depth + 1]:
                    e = node.setdefault(f, [0.0, {}])
                    e[0] += w
                    node = e[1]
        def show(node, ind, parent):
            for f, (c, sub) in sorted(node.items(), key=lambda x: -x[1][0]):
                if parent and c < parent * minp / 100:
                    continue
                print("%s%7.3f  %s" % ("  " * ind, c, pr.name(f)))
                show(sub, ind + 1, parent or c)
        print("arbol (ms/fot, media de %d escenas), profundidad %d, minimo %.1f %% de la raiz" % (len(scenes), depth, minp))
        show(tree, 0, None)
        return

    def fase_of(ch):
        """innermost root wins"""
        for f in reversed(ch):
            ph = ROOT2PH.get(f)
            if ph is not None:
                return ph
        return "(sin fase)"

    ROOT2PH = {}
    for name, roots in FASES:
        for a in roots:
            ROOT2PH[a] = name

    if cmd == "fases":
        names = [n for n, _ in FASES] + ["(sin fase)"]
        tab = {}
        for s in scenes:
            rs = by_scene[s]
            w = per_frame_ms(rs)
            c = collections.Counter()
            for r in rs:
                ch = pr.sample_chain(r)
                if is_idle(pr, ch, r):
                    c["(ocioso / espera)"] += w
                    continue
                key = fase_of(ch)
                if not (r[9] & 1) and key == "(sin fase)":
                    key = "(otro hilo invitado, sin fase)"
                c[key] += w
            tab[s] = c
        allk = []
        for n in names + ["(otro hilo invitado, sin fase)", "(ocioso / espera)"]:
            if any(tab[s].get(n) for s in scenes) and n not in allk:
                allk.append(n)
        print("%-44s" % "fase (ms por fotograma del juego)" + "".join("%12s" % scene_name(s)[:11] for s in scenes))
        for n in allk:
            print("%-44s" % n[:44] + "".join("%12.2f" % tab[s].get(n, 0) for s in scenes))
        print("%-44s" % "TOTAL ocupado" + "".join("%12.2f" % sum(v for k, v in tab[s].items() if k != "(ocioso / espera)") for s in scenes))
        print("%-44s" % "fotograma (ms de pared)" + "".join("%12.2f" % sum(tab[s].values()) for s in scenes))
        return

    def subsis_of(ch):
        for f in reversed(ch):
            if isinstance(f, str):
                continue
            for lo, hi, n in SUBSIS:
                if lo <= f < hi:
                    return n
            return "otro invitado"
        return "runtime (sin funcion invitada)"

    if cmd == "cruce":
        # fase x subsistema, media de las escenas elegidas (ms/fot)
        tab = collections.defaultdict(collections.Counter)
        cols = collections.Counter()
        for s in scenes:
            rs = by_scene[s]
            w = per_frame_ms(rs) / len(scenes)
            for r in rs:
                ch = pr.sample_chain(r)
                if is_idle(pr, ch, r):
                    continue
                ph = fase_of(ch)
                if not (r[9] & 1) and ph == "(sin fase)":
                    ph = "(otro hilo invitado, sin fase)"
                su = subsis_of(ch)
                tab[ph][su] += w
                cols[su] += w
        cl = [c for c, _ in cols.most_common()]
        print("fase \\ subsistema (ms/fot, media de: %s)" % ", ".join(scene_name(s) for s in scenes))
        for i, c in enumerate(cl):
            print("  [%d] %s" % (i, c))
        print("%-46s %6s " % ("", "TOTAL") + "".join("%6s" % ("[%d]" % i) for i in range(len(cl))))
        order = [n for n, _ in FASES] + ["(sin fase)", "(otro hilo invitado, sin fase)"]
        for ph in order:
            if ph not in tab:
                continue
            print("%-46s %6.2f " % (ph[:46], sum(tab[ph].values())) + "".join(
                ("%6.2f" % tab[ph][c]) if tab[ph][c] >= 0.005 else "     ." for c in cl))
        print("%-46s %6.2f " % ("TOTAL", sum(cols.values())) + "".join("%6.2f" % cols[c] for c in cl))
        return

    if cmd == "hilos":
        # tprof_host.bin: muestras de los otros hilos del anfitrion (0 render "main", 4 trabajador "gxw")
        hp = os.path.join(d, "tprof_host.bin")
        raw = open(hp, "rb").read()
        hrecs = list(struct.iter_unpack("<IIIIHbB", raw[:len(raw) // 20 * 20]))
        names = {0: "render (nucleo 0)", 4: "trabajador gxw (nucleo 2)", 2: "ax", 3: "audio"}
        def kind(pc, lr, hs):
            s0 = pr.hsym(pc) if pc != 0xFFFFFFFF else None
            chain = [s0 or "?"]
            if hs and hs < len(pr.stacks):
                chain += [str(pr.hsym(a)) for a in pr.stacks[hs][1:]]
            else:
                chain.append(str(pr.hsym(lr)))
            j = " ".join(chain)
            if "GX_HostWait" in j: return "ESPERA: sin fotograma que dibujar (ocioso)"
            if "nvFenceWait" in j or "FenceWait" in j: return "ESPERA: a la GPU (fence de nouveau)"
            if "nwindow" in j or "Dequeue" in j or "bqDequeue" in j: return "ESPERA: buffer de pantalla (vsync)"
            if s0 in ("svcSleepThread",): return "ESPERA: dormido (nanosleep)"
            if s0 in ("svcWaitProcessWideKeyAtomic", "svcArbitrateLock", "svcWaitSynchronization", "svcWaitForAddress"):
                return "ESPERA: " + " < ".join(chain[1:4])
            if s0 == "svcSendSyncRequest": return "nucleo: " + chain[1]
            if s0 is None: return "CPU: ? (sin simbolo)"
            if "nwii" in s0 or s0.startswith("_ZL") or "GXCommand" in s0: return "CPU runtime: " + s0[:70]
            return "CPU driver/libc: " + s0[:60]
        for s in scenes:
            t_lo = min(r[0] for r in by_scene[s]) if by_scene[s] else 0
            fr = frames_of(by_scene[s])
            wall = wall_of(by_scene[s]) / 1000.0
            for slot in sorted(names):
                hs_ = [h for h in hrecs if h[5] == s and h[6] == slot and h[0] >= t_lo]
                if not hs_:
                    continue
                c = collections.Counter()
                grp = collections.Counter()
                for h in hs_:
                    k = kind(h[1], h[2], h[3])
                    c[k] += 1
                    grp[k.split(":")[0]] += 1
                n = len(hs_)
                print("\n== %s | hilo %s: %d muestras; por fotograma del juego (%.1f ms): " % (scene_name(s), names[slot], n, wall / fr)
                      + ", ".join("%s %.1f%%" % (k, 100.0 * v / n) for k, v in grp.most_common(6)))
                for k, v in c.most_common(int(rest[0]) if rest else 14):
                    print("  %5.1f%%  %6.2f ms/fot juego  %s" % (100.0 * v / n, v / n * wall / fr, k))
        return

    if cmd == "esperas":
        for s in scenes:
            rs = by_scene[s]
            w = per_frame_ms(rs)
            c = collections.Counter()
            hc = collections.Counter()
            for r in rs:
                ch = pr.sample_chain(r)
                idle = is_idle(pr, ch, r)
                if not (r[9] & 1):
                    ch2 = pr.chain_of(r[4], None)
                    st = pr.stacks[r[4]] if r[4] < len(pr.stacks) else ()
                    inner = [pr.gfun(a) for a in st[:2]] if st else []
                    key = ("ocioso" if idle else "otro hilo corre") + " | principal en: " + " < ".join(
                        pr.name(f) for f in ([x for x in inner[:1] if x] + list(reversed(ch2))[:5]))
                    c[key] += w
                elif idle:
                    c["ocioso con el hilo principal como actual: " + " < ".join(pr.name(f) for f in reversed(ch[-4:]))] += w
                if r[6]:
                    st = pr.stacks[r[6]]
                    hc[" < ".join([ch[-1] if isinstance(ch[-1], str) else "?"] + [str(pr.hsym(a)) for a in st[1:5]])] += w
            print("\n== %s: donde esta el hilo principal cuando no corre (ms/fot)" % scene_name(s))
            for k, v in c.most_common(14):
                print("  %7.3f  %s" % (v, k[:230]))
            print("   -- esperas del anfitrion (pc en una llamada al nucleo): ms/fot")
            for k, v in hc.most_common(10):
                print("  %7.3f  %s" % (v, k[:230]))
        return

    if cmd == "hist":
        # [r35m4] por fotograma del juego (entre copias XFB): ms OCUPADOS del hilo principal (muestras no ociosas
        # con r[9]&1) y ms de pared; histograma y cuantos pasan de 16,7 ms.
        for s in scenes:
            rs = by_scene[s]
            fr = []; p = None; t0 = None; busy = 0; prev_t = None
            for r in rs:
                if prev_t is not None and r[0] - prev_t > 50000:
                    p = None
                prev_t = r[0]
                if p is None or r[7] != p:
                    if p is not None:
                        fr.append(((r[0] - t0) / 1000.0, busy * us / 1000.0))
                    p = r[7]; t0 = r[0]; busy = 0
                if r[9] & 1:
                    ch = pr.sample_chain(r)
                    if not is_idle(pr, ch, r):
                        busy += 1
            if not fr:
                continue
            n = len(fr)
            B = [0, 10, 12, 14, 16.7, 18, 20, 25, 33.3, 1e9]
            def h(vals):
                c = [0] * (len(B) - 1)
                for v in vals:
                    for i in range(len(B) - 1):
                        if B[i] <= v < B[i + 1]:
                            c[i] += 1; break
                return " ".join("%5.1f" % (100.0 * x / n) for x in c)
            ws = sorted(f[0] for f in fr); bs = sorted(f[1] for f in fr)
            print("\n== %s: %d fotogramas | pared mediana %.1f ms, ocupado mediana %.1f p90 %.1f ms | ocupado > 16,7 ms: %.1f %% | pared > 18 ms: %.1f %%" % (
                scene_name(s), n, ws[n // 2], bs[n // 2], bs[n * 9 // 10], 100.0 * sum(1 for b in bs if b > 16.7) / n, 100.0 * sum(1 for w in ws if w > 18) / n))
            print("   %% por tramo (ms):       <10   10-12 12-14 14-16.7 -18 18-20 20-25 25-33 >33")
            print("   ocupado:              " + h(bs))
            print("   pared:                " + h(ws))
        return
    if cmd == "linea":
        # Linea de tiempo media del fotograma. Fotograma = entre dos copias XFB (fin de endRender).
        # Por fase: en cuantos fotogramas aparece, ms medios, y ms medios (desde la copia XFB) de su
        # primera y ultima muestra. Las fases del hilo principal salen en su orden real.
        for s in scenes:
            rs = by_scene[s]
            agg = collections.defaultdict(lambda: [0, 0.0, 0.0, 0.0])  # frames, samples, sum first, sum last
            cur = {}
            p = None
            t0 = None
            nfr = 0
            lens = []

            def close():
                for k, (a, b, n) in cur.items():
                    g = agg[k]
                    g[0] += 1
                    g[1] += n
                    g[2] += a
                    g[3] += b
            prev_t = None
            for r in rs:
                if prev_t is not None and r[0] - prev_t > 50000:
                    cur = {}
                    p = None
                prev_t = r[0]
                if p is None or r[7] != p:
                    if p is not None:
                        close()
                        nfr += 1
                        lens.append((r[0] - t0) / 1000.0)
                    cur = {}
                    p = r[7]
                    t0 = r[0]
                ch = pr.sample_chain(r)
                if is_idle(pr, ch, r):
                    k = "(ocioso: espera al retrazo)"
                else:
                    k = fase_of(ch)
                    if not (r[9] & 1) and k == "(sin fase)":
                        k = "(otro hilo invitado, sin fase)"
                dt = (r[0] - t0) / 1000.0
                e = cur.get(k)
                if e is None:
                    cur[k] = [dt, dt, 1]
                else:
                    e[1] = dt
                    e[2] += 1
            if not nfr:
                continue
            lens.sort()
            w = wall_of(rs) / 1000.0 / len(rs)
            print("\n== %s: %d fotogramas; duracion mediana %.1f ms (p10 %.1f, p90 %.1f)" % (
                scene_name(s), nfr, lens[len(lens) // 2], lens[len(lens) // 10], lens[len(lens) * 9 // 10]))
            print("   %-46s %8s %8s %10s %10s" % ("fase", "% fotogr", "ms/fot", "empieza ms", "acaba ms"))
            for k, g in sorted(agg.items(), key=lambda x: x[1][2] / max(1, x[1][0])):
                print("   %-46s %7.0f%% %8.2f %10.1f %10.1f" % (k[:46], 100.0 * g[0] / nfr, g[1] * w / nfr, g[2] / g[0], g[3] / g[0]))
        return
    print(__doc__)


if __name__ == "__main__":
    main()
