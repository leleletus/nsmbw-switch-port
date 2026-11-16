#!/usr/bin/env python3
"""[r44w] tprof_w.py: reparto del tiempo PROPIO del hilo del juego (NWII_TPROF) por categoría.

  tprof_w.py <bench> <nm> propio [N] [--escena agua_pinguino]
  tprof_w.py <bench> <nm> rtfase [--escena ...]      runtime (rt:) por fase del fotograma

Reutiliza r9/r35m4/tools/tprof_m4.py (Prof, fases). Categorías del pc del anfitrión en las muestras ocupadas:
  invitado   función generada (func_8xxxxxxx_g6…; incluye lo que el código generado lleva en línea)
  rt         símbolo del runtime nwii:: (u otra C++ del port)
  libc       memcpy/memset/malloc/str*/… de newlib, libstdc++
  driver     libnx / mesa / nouveau / svc* (llamadas al núcleo que no son esperas)
"""
import collections
import importlib.util
import os
import re
import sys

T = "/home/djvemo/nsmbw-port/r9/r35m4/tools/tprof_m4.py"
spec = importlib.util.spec_from_file_location("tprof_m4", T)
tp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tp)

LIBC = re.compile(r"^(memcpy|memmove|memset|memcmp|__memcpy|_?malloc|_?free|_?calloc|_?realloc|str\w+|_?__?\w*_r$|"
                  r"__aeabi|__udiv|__div|_Znwm|_ZdlPv|_ZdlPvm|_Znam|_ZdaPv|__cxa|__gxx|_ZNSt|_ZSt|__libc|"
                  r"__retarget|__sfvwrite|vfprintf|_vfprintf|__ssputs|_printf|snprintf|vsnprintf|fwrite|fflush|"
                  r"__s|__lock|__unlock|_lock|_unlock|__syscall|pthread_|__aarch64|sqrt|sin|cos|atan|exp|pow|fmod|"
                  r"floor|ceil|ldexp|frexp|_?tls|__tls)")
DRV = re.compile(r"^(svc|arm|nv|gl|_mesa|mesa|nouveau|egl|_egl|vi|hos|nx_|_nx|ipc|serviceIpc|tipc|cmif|"
                 r"__nx|audren|audout|hid|fs|fsdev|virtmem|mutex|rmutex|condvar|barrier|semaphore|thread|"
                 r"armGetSystemTick|leventWait|ueventWait|waitMulti|_?pthread|__syscall)")


def cat(name):
    if name is None:
        return "?"
    if not name.startswith("rt:"):
        return "invitado"
    s = name[3:]
    if s == "?":
        return "rt?"
    if s.startswith("_ZN4nwii") or s.startswith("_ZL") or s.startswith("_ZN") and "nwii" in s:
        return "rt"
    if LIBC.match(s):
        return "libc"
    if DRV.match(s):
        return "driver"
    if s.startswith("_Z"):
        return "rt"
    return "otros:" + s.split("_")[0][:12]


def demangle_short(s):
    m = re.match(r"rt:_ZN4nwii(?:7runtime|2gx|3gfx)?(?:L)?(\d+)", s)
    if m:
        n = int(m.group(1))
        rest = s[m.end():]
        return "rt:" + rest[:n]
    m = re.match(r"rt:_ZN4nwii7runtime(\d+)(\w+?)(\d+)", s)
    return s


def main():
    argv = sys.argv
    esc = tp.args_opt("--escena")
    d, nm, cmd = argv[1], argv[2], argv[3]
    rest = argv[4:]
    fasesf = os.path.join(os.path.dirname(T), "fases.py")
    ns = {}
    exec(open(fasesf).read(), ns)
    tp.NOMBRES.update(ns.get("NOMBRES", {}))
    tp.FASES[:] = ns.get("FASES", [])
    pr = tp.Prof(d, nm, tp.P + "/game/SMNE01/sys/main.dol", tp.P + "/symbols/usa_symbols.txt")
    recs = pr.recs
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
    scenes = [s for s in by_scene if s >= 0]
    if esc is not None:
        scenes = [s for s in scenes if tp.scene_name(s) == esc or str(s) == esc]

    def wf(rs):
        n = 0
        w = 0
        for i in range(1, len(rs)):
            dt = rs[i][0] - rs[i - 1][0]
            if 0 < dt < 50000:
                n += (rs[i][7] - rs[i - 1][7]) & 0xFFFF
                w += dt
        return (w / 1000.0 / len(rs)) / max(n, 1), max(n, 1)

    # fase de una cadena: raíz más interna que pertenezca a una fase (como tprof_m4 'fases')
    froots = {}
    for i, (fname, roots) in enumerate(tp.FASES):
        for x in roots:
            froots.setdefault(x, (i, fname))

    def fase(ch):
        best = None
        for f in ch:
            if f in froots:
                best = froots[f]
        return best[1] if best else "(sin fase)"

    N = int(rest[0]) if rest else 40
    for s in scenes:
        rs = by_scene[s]
        w, fr = wf(rs)
        cc = collections.Counter()
        sym = collections.Counter()
        rtf = collections.defaultdict(collections.Counter)
        par = collections.defaultdict(collections.Counter)
        busy = 0
        for r in rs:
            ch = pr.sample_chain(r)
            if tp.is_idle(pr, ch, r) or not (r[9] & 1):
                continue
            busy += 1
            leaf = ch[-1]
            nm_ = leaf if isinstance(leaf, str) else None
            c = cat(nm_) if nm_ else "invitado"
            cc[c] += 1
            if c != "invitado":
                sym[nm_] += 1
                ph = fase(ch)
                rtf[ph][c] += 1
                # quién llama: función invitada padre
                p = None
                for f in reversed(ch[:-1]):
                    if not isinstance(f, str):
                        p = f
                        break
                par[nm_][p] += 1
        print("\n== %s: %d fotogramas, hilo principal ocupado %.2f ms/fot" % (tp.scene_name(s), fr, busy * w))
        if cmd == "propio":
            for c, n in cc.most_common():
                print("   %-22s %6.2f ms/fot  %5.1f %%" % (c, n * w, 100.0 * n / busy))
            print("  -- símbolos fuera del código generado (propio, ms/fot) y su llamador invitado principal")
            for k, n in sym.most_common(N):
                pp = par[k].most_common(2)
                pps = ", ".join("%s %.0f%%" % (pr.name(f) if f is not None else "-", 100.0 * m / n) for f, m in pp)
                print("   %6.3f  %-8s %s   [%s]" % (n * w, cat(k), k[:90], pps[:110]))
        elif cmd == "wgp":
            # escritura al pipe de GX: por función invitada que escribe (y su llamador), ms/fot
            WG = re.compile(r"GX_WGPIPE|write(8|16|32|_f32)_slow|wgp_write32|wg_flush_now|psq_st_gen")
            agg = collections.Counter()
            agg_in = collections.Counter()
            tot = 0
            for r in rs:
                ch = pr.sample_chain(r)
                if tp.is_idle(pr, ch, r) or not (r[9] & 1):
                    continue
                leaf = ch[-1]
                g = [f for f in ch if not isinstance(f, str)]
                if isinstance(leaf, str) and WG.search(leaf):
                    tot += 1
                    k = tuple(g[-2:]) if g else ()
                    agg[k] += 1
                # inclusivo de las funciones que escriben (todo su tiempo)
            print("   escritura al pipe (propio del runtime): %.3f ms/fot" % (tot * w))
            for k, n in agg.most_common(N):
                print("   %6.3f  %s" % (n * w, " < ".join(pr.name(f) for f in reversed(k))[:150]))
        elif cmd == "rtfase":
            for ph, c in sorted(rtf.items(), key=lambda x: -sum(x[1].values())):
                print("   %-44s %s" % (ph[:44], "  ".join("%s %.2f" % (k, v * w) for k, v in c.most_common())))


if __name__ == "__main__":
    main()
