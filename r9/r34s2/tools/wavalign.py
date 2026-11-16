#!/usr/bin/env python3
# wavalign.py a.wav b.wav [secs=60]: align two NWII_AUDIODUMP files at their first non-silent frame and
# compare the next <secs> seconds sample by sample (runs whose sound starts at a different host moment).
import sys
def load(p):
    d = open(p, 'rb').read()[44:]
    return d[:len(d) // 4 * 4]
a, b = load(sys.argv[1]), load(sys.argv[2])
secs = float(sys.argv[3]) if len(sys.argv) > 3 else 60
def onset(d):
    z = b'\0' * 4096
    i = 0
    while i < len(d) and d[i:i + 4096] == z[:len(d[i:i + 4096])]: i += 4096
    while i < len(d) and d[i:i + 4] == b'\0\0\0\0': i += 4
    return i
oa, ob = onset(a), onset(b)
n = min(len(a) - oa, len(b) - ob, int(secs * 32000) * 4) // 4 * 4
x, y = a[oa:oa + n], b[ob:ob + n]
print(f"onset {oa / 4 / 32000:.3f} s / {ob / 4 / 32000:.3f} s; comparing {n / 4 / 32000:.1f} s")
if x == y:
    print("IDENTICAL after alignment"); sys.exit(0)
j = next(i for i in range(0, n, 4) if x[i:i + 4] != y[i:i + 4])
print(f"DIFFER after alignment: first at +{j / 4 / 32000:.3f} s, {sum(1 for i in range(0, n, 4) if x[i:i+4] != y[i:i+4])} frames differ")
sys.exit(1)
