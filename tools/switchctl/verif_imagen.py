#!/usr/bin/env python3
"""verif_imagen.py <bench dir> [ref dir ...]: automatic image check of a console run against clean reference runs.

Every screenshot 02_title_*.jpg of the run is compared with ALL the screenshots of the reference runs (same fixed demo
list): thumbnail 96x54 RGB without the overlays (fps box top-left, status-monitor bar at the bottom), nearest
neighbour by mean absolute difference. A shot is SUSPICIOUS when its nearest clean shot is far (texture corruption,
wrong colours, missing models) or when it has much more black than its nearest clean shot (black models/textures).
Fades and loading screens (almost all black / all one colour) are skipped.
Default references: runs listed in tools/switchctl/verif_refs.txt (clean runs checked by eye).
Output: one line per suspicious shot, a summary line "IMAGEN OK|SOSPECHOSA ..." and verif_imagen.txt in the bench dir.
Exit code 0 = ok, 2 = suspicious, 1 = could not check."""
import glob, os, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
LOGS = os.path.normpath(os.path.join(HERE, "../../switch/real_logs"))
W, H = 96, 54
T_DIST = float(os.environ.get("VERIF_DIST", "25"))     # mean abs diff (0-255) to the nearest clean shot
T_BLACK = float(os.environ.get("VERIF_BLACK", "0.07"))  # extra fraction of black pixels over the nearest clean shot
MAX_BAD = float(os.environ.get("VERIF_MAX_BAD", "0.18"))  # clean runs: <= 11 % (new scenes, cutscenes); corrupted: 50 %+


def thumb(path):
    try:
        im = Image.open(path).convert("RGB")
    except Exception:
        return None
    if im.size != (1280, 720):
        im = im.resize((1280, 720))
    im = im.crop((0, 0, 1280, 680))            # drop the status-monitor bar
    im.paste((0, 0, 0), (0, 0, 205, 130))      # fps box
    t = np.asarray(im.resize((W, H), Image.BILINEAR), dtype=np.int16)
    black = float((t.max(axis=2) < 24).mean())
    return t.reshape(-1), black, float(t.var())


def dist(a, b):
    return float(np.abs(a - b).mean())


def shots(d):
    return sorted(glob.glob(os.path.join(d, "02_title_*.jpg")))


def main():
    if len(sys.argv) < 2:
        print(__doc__); return 1
    run = sys.argv[1].rstrip("/")
    refs = sys.argv[2:]
    if not refs:
        f = os.path.join(HERE, "verif_refs.txt")
        refs = [os.path.join(LOGS, l.strip()) for l in open(f) if l.strip() and not l.startswith("#")] if os.path.exists(f) else []
    ref = []
    for d in refs:
        for p in shots(d):
            t = thumb(p)
            if t and t[1] < 0.85 and t[2] > 150:
                ref.append((t, p))
    mine = shots(run)
    if not ref or not mine:
        print(f"IMAGEN SIN COMPROBAR: {len(mine)} capturas, {len(ref)} de referencia"); return 1
    bad, n, lines = [], 0, []
    for p in mine:
        t = thumb(p)
        if not t or t[1] > 0.85 or t[2] < 150:   # fade / loading / unreadable
            continue
        n += 1
        d, rb, rp = min((dist(t[0], r[0][0]), r[0][1], r[1]) for r in ref)
        extra = t[1] - rb
        if d > T_DIST or extra > T_BLACK:
            bad.append(p)
            lines.append(f"  SOSPECHOSA {os.path.basename(p)}: dist {d:.1f}, negro {t[1]*100:.0f}% (ref {rb*100:.0f}%) ~ {os.path.basename(os.path.dirname(rp))}/{os.path.basename(rp)}")
    frac = len(bad) / max(n, 1)
    ok = frac <= MAX_BAD
    summary = f"IMAGEN {'OK' if ok else 'SOSPECHOSA'}: {len(bad)} de {n} capturas raras ({frac*100:.0f} %), {len(ref)} de referencia"
    # 15-nov: frozen game (Z22: m5 hung on the iris between demos and the bench reported rc 0). Four or more
    # consecutive byte-identical captures (40 s with SHOT_EVERY=10) = hang.
    # congelada: capturas seguidas iguales SIN contar el recuadro del medidor (arriba a la izquierda),
    # que sigue cambiando aunque el juego este colgado (D07, 15-nov)
    def cuerpo(p):
        try:
            a = np.asarray(Image.open(p).convert("L"), dtype=np.int16)
        except Exception:
            return None
        a = a.copy()
        a[: a.shape[0] // 5, : a.shape[1] // 5] = 0
        return a

    cs = [cuerpo(p) for p in mine]
    hs = [0]
    for i in range(1, len(cs)):
        igual = (cs[i] is not None and cs[i - 1] is not None and cs[i].shape == cs[i - 1].shape
                 and float(np.abs(cs[i] - cs[i - 1]).mean()) < 0.6)
        hs.append(hs[-1] if igual else hs[-1] + 1)
    best, cur, at = 1, 1, 0
    for i in range(1, len(hs)):
        cur = cur + 1 if hs[i] == hs[i - 1] else 1
        if cur > best:
            best, at = cur, i - cur + 1
    if best >= 4:
        ok = False
        lines.append(f"  CONGELADA: {best} capturas seguidas identicas desde {os.path.basename(mine[at])}")
        summary = f"IMAGEN SOSPECHOSA: CONGELADA ({best} capturas identicas desde {os.path.basename(mine[at])}); " + summary
    out = "\n".join(lines + [summary])
    print(out)
    try:
        open(os.path.join(run, "verif_imagen.txt"), "w").write(out + "\n")
    except OSError:
        pass
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
