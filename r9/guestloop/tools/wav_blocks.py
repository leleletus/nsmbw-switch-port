#!/usr/bin/env python3
"""wav_blocks.py a.wav b.wav [block_s=0.5] (R9 guestloop): how much of A's audio appears verbatim in B.

Two runs of the same scene start their music a few frames apart and drift (scene changes depend on
loading time), so a single global shift does not line them up. For every non-silent block of A this
looks for the same samples anywhere in B (exact byte match of the whole block, frame-aligned) and
reports the share of blocks found and the spread of their shifts."""
import sys, wave
import numpy as np

def load(p):
    with wave.open(p, 'rb') as w:
        return w.readframes(w.getnframes()), w.getframerate(), w.getnchannels()

A, rate, ch = load(sys.argv[1]); B, _, _ = load(sys.argv[2])
bs = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
fb = ch * 2; blk = int(rate * bs) * fb
found = silent = total = 0; shifts = []
for off in range(0, len(A) - blk, blk):
    a = A[off:off + blk]
    s = np.frombuffer(a, dtype='<i2')
    if np.abs(s).max() < 64:
        silent += 1; continue
    total += 1
    pos = B.find(a)
    while pos != -1 and pos % fb:
        pos = B.find(a, pos + 1)
    if pos != -1:
        found += 1; shifts.append((pos - off) / fb / rate * 1000)
print(f"{sys.argv[1].split('/')[-1]} vs {sys.argv[2].split('/')[-1]}: {total} non-silent {bs:g}-s blocks "
      f"({silent} silent): {found} found verbatim in B ({100*found/max(total,1):.1f}%)"
      + (f", shift ms min {min(shifts):.0f} median {sorted(shifts)[len(shifts)//2]:.0f} max {max(shifts):.0f}" if shifts else ""))
