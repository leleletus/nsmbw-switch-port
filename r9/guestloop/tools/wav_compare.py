#!/usr/bin/env python3
"""wav_compare.py a.wav b.wav [max_shift_s=2] (R9 guestloop): compare two NWII_AUDIODUMP files.

Both runs play the same game audio at 32 kHz, but the dumps can start a few ms apart. Finds the
shift (in stereo frames, |shift| <= max_shift_s) at which the most 1-s blocks are bit-identical,
then reports, per 1-s block, whether the samples are identical, and the first block that differs.
Needs numpy."""
import sys, wave
import numpy as np

def load(p):
    with wave.open(p, 'rb') as w:
        assert w.getsampwidth() == 2
        n = w.getnframes(); ch = w.getnchannels(); rate = w.getframerate()
        a = np.frombuffer(w.readframes(n), dtype='<i2').reshape(-1, ch)
    return a, rate

a, ra = load(sys.argv[1]); b, rb = load(sys.argv[2])
assert ra == rb
rate = ra
max_shift = int(float(sys.argv[3]) * rate) if len(sys.argv) > 3 else 2 * rate
# Find the shift: first non-silent frame of each, then refine by exact-match search around it.
def first_sound(x):
    nz = np.nonzero(np.abs(x).sum(axis=1) > 64)[0]
    return int(nz[0]) if len(nz) else 0
fa, fb = first_sound(a), first_sound(b)
best = None
for base in [fb - fa]:
    for d in range(-64, 65):
        s = base + d
        if abs(s) > max_shift: continue
        # compare 0.5 s right after the first sound in a
        i0 = fa; j0 = fa + s
        if j0 < 0 or i0 + rate // 2 > len(a) or j0 + rate // 2 > len(b): continue
        eq = np.mean(np.all(a[i0:i0 + rate // 2] == b[j0:j0 + rate // 2], axis=1))
        if best is None or eq > best[1]: best = (s, eq)
if best is None:
    print('no alignment'); sys.exit(1)
s = best[0]
i0 = max(0, -s); j0 = i0 + s
n = min(len(a) - i0, len(b) - j0)
A = a[i0:i0 + n]; B = b[j0:j0 + n]
blocks = n // rate
same = [bool(np.all(A[k * rate:(k + 1) * rate] == B[k * rate:(k + 1) * rate])) for k in range(blocks)]
first_diff = next((k for k, v in enumerate(same) if not v), None)
eqf = float(np.mean(np.all(A == B, axis=1))) if n else 0.0
print(f"a: {len(a)/rate:.1f} s, b: {len(b)/rate:.1f} s, first sound a {fa/rate:.3f} s b {fb/rate:.3f} s, "
      f"shift {s} frames ({s/rate*1000:.1f} ms), match at start {best[1]*100:.1f}%")
print(f"overlap {n/rate:.1f} s: identical frames {eqf*100:.2f}%, identical 1-s blocks {sum(same)}/{blocks}, "
      f"first differing block: {'none' if first_diff is None else f'{first_diff} s after the start of the overlap'}")
print('blocks: ' + ''.join('=' if v else 'x' for v in same))
