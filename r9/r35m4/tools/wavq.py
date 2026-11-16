#!/usr/bin/env python3
# wavq.py a.wav [b.wav] : calidad de un volcado NWII_AUDIODUMP y comparacion por ventanas de dos volcados.
#  Un archivo: duracion, comienzo del sonido, nivel, huecos (>= 1 ms de ceros exactos entre sonido), clics
#  (saltos de muestra muy por encima del entorno; aparte los que caen en la costura de una trama AX de 96 muestras).
#  Dos archivos: los alinea por la envolvente (las corridas no son deterministas en el tiempo), y por ventanas
#  de 1 s compara nivel (dB) y espectro en 30 bandas (correlacion de las bandas en dB).
import sys, numpy as np
SR = 32000
def load(p):
    d = np.frombuffer(open(p, 'rb').read()[44:], dtype='<i2')
    d = d[:len(d) // 2 * 2].reshape(-1, 2).astype(np.float64)
    return d
def one(p, d):
    m = d.mean(axis=1); n = len(m)
    nz = np.flatnonzero(np.abs(d).sum(axis=1) > 0)
    on = nz[0] if len(nz) else n
    print(f"{p}: {n / SR:.2f} s, sonido desde {on / SR:.2f} s, rms {np.sqrt((m ** 2).mean()):.0f}, pico {np.abs(d).max():.0f}, "
          f"saturadas {(np.abs(d) >= 32767).sum()}")
    # huecos: tramos de ceros exactos de 1-200 ms con sonido antes y despues (20 ms)
    z = (np.abs(d).sum(axis=1) == 0).astype(np.int8)
    e = np.diff(np.concatenate(([0], z, [0]))); st = np.flatnonzero(e == 1); en = np.flatnonzero(e == -1)
    env = np.abs(m)
    gaps = 0; gl = []
    for s, t in zip(st, en):
        L = t - s
        if 32 <= L <= 6400 and s > 640 and t + 640 < n and env[s - 640:s].mean() > 30 and env[t:t + 640].mean() > 30:
            gaps += 1; gl.append((s / SR, L))
    # clics
    dm = np.abs(np.diff(m)); k = 96
    pad = np.pad(dm, (k, k), mode='edge'); cs = np.cumsum(np.concatenate(([0.0], pad)))
    loc = (cs[2 * k + 1:] - cs[:-2 * k - 1]) / (2 * k + 1)
    click = np.flatnonzero((dm > 12 * (loc + 40)) & (dm > 3000)) + 1
    seam = int(((click - on) % 96 == 0).sum())
    print(f"   huecos {gaps} {['%.2fs:%d' % g for g in gl[:6]]}  clics {len(click)} (en costura de trama AX: {seam}) "
          f"{['%.2f' % (c / SR) for c in click[:8]]}")
    return on, gaps, len(click)
def bands(x):
    # espectro medio (Welch, 2048) en 30 bandas log entre 60 Hz y 15 kHz, en dB
    N = 2048; w = np.hanning(N); nf = (len(x) - N) // (N // 2) + 1
    if nf < 1: return None
    idx = np.arange(N)[None, :] + (N // 2) * np.arange(nf)[:, None]
    P = (np.abs(np.fft.rfft(x[idx] * w, axis=1)) ** 2).mean(axis=0)
    f = np.fft.rfftfreq(N, 1 / SR); ed = np.geomspace(60, 15000, 31)
    return np.array([10 * np.log10(P[(f >= ed[i]) & (f < ed[i + 1])].sum() + 1.0) for i in range(30)])
def envelope(m, hop=320):
    n = len(m) // hop
    return np.sqrt((m[:n * hop].reshape(n, hop) ** 2).mean(axis=1))
a = load(sys.argv[1]); ra = one(sys.argv[1], a)
if len(sys.argv) > 2:
    b = load(sys.argv[2]); rb = one(sys.argv[2], b)
    ma, mb = a.mean(axis=1), b.mean(axis=1)
    ea, eb = np.log1p(envelope(ma)), np.log1p(envelope(mb))
    n = min(len(ea), len(eb)); best = (-1, 0)
    for lag in range(-1000, 1001):  # +-10 s en pasos de 10 ms
        x = ea[max(0, lag):n + min(0, lag)]; y = eb[max(0, -lag):n - max(0, lag)]
        k = min(len(x), len(y))
        if k < 500: continue
        x = x[:k] - x[:k].mean(); y = y[:k] - y[:k].mean()
        c = (x * y).sum() / (np.sqrt((x * x).sum() * (y * y).sum()) + 1e-9)
        if c > best[0]: best = (c, lag)
    c, lag = best; off = lag * 320
    print(f"alineacion: B va {-lag * 10} ms respecto a A (correlacion de envolvente {c:.3f})")
    sa = max(0, off); sb = max(0, -off); L = min(len(ma) - sa, len(mb) - sb)
    rows = []
    for w in range(L // SR):
        xa = ma[sa + w * SR: sa + (w + 1) * SR]; xb = mb[sb + w * SR: sb + (w + 1) * SR]
        la = 10 * np.log10((xa ** 2).mean() + 1); lb = 10 * np.log10((xb ** 2).mean() + 1)
        if la < 20 and lb < 20: rows.append((w, la, lb, 1.0)); continue
        ba, bb = bands(xa), bands(xb)
        cc = np.corrcoef(ba, bb)[0, 1] if ba.std() > 0 and bb.std() > 0 else 0.0
        rows.append((w, la, lb, cc))
    r = np.array(rows)
    dl = np.abs(r[:, 1] - r[:, 2])
    print(f"ventanas de 1 s: {len(r)}; |dif. de nivel| mediana {np.median(dl):.2f} dB, p90 {np.percentile(dl, 90):.2f}, max {dl.max():.2f}; "
          f"correlacion espectral mediana {np.median(r[:, 3]):.3f}, p10 {np.percentile(r[:, 3], 10):.3f}, min {r[:, 3].min():.3f}; "
          f"ventanas con corr < 0.90: {(r[:, 3] < 0.90).sum()}, con dif. de nivel > 3 dB: {(dl > 3).sum()}")
    bad = [(int(w), round(la, 1), round(lb, 1), round(cc, 2)) for w, la, lb, cc in rows if cc < 0.9 or abs(la - lb) > 3]
    if bad: print("   peores (s, dB A, dB B, corr):", bad[:12])
