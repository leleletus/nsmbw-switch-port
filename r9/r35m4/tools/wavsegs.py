#!/usr/bin/env python3
# wavsegs.py a.wav b.wav: piecewise alignment. Walks a in 1 s blocks; each block (if not silent) is looked up
# in b near the current offset (exact byte match, search +-20 s by frame steps of whole blocks first via a
# hash of the block). Reports matched seconds, unmatched seconds and the offsets used.
import sys, hashlib
def load(p):
    d = open(p, 'rb').read()[44:]
    return d[:len(d) // 4 * 4]
a, b = load(sys.argv[1]), load(sys.argv[2])
B = 32000 * 4  # 1 s
# index of every 256-byte-aligned position of b keyed by its first 64 bytes is too big; use a rolling search instead
pos = {}
step = 4
key = 64
for i in range(0, len(b) - key, step):
    k = b[i:i + key]
    if k != b'\0' * key and k not in pos: pos[k] = i
matched = silent = unmatched = 0
offs = []
for s in range(0, len(a) // B):
    blk = a[s * B:(s + 1) * B]
    if blk == b'\0' * B: silent += 1; continue
    nz = next(i for i in range(0, B, 4) if blk[i:i + 4] != b'\0\0\0\0')
    k = blk[nz:nz + key]
    j = pos.get(k)
    if j is not None and b[j - nz:j - nz + B] == blk if j - nz >= 0 else False:
        matched += 1
        o = (j - nz - s * B) // 4
        if not offs or offs[-1][1] != o: offs.append((s, o))
    else:
        unmatched += 1
print(f"a: {len(a)//B} s; matched {matched} s, silent {silent} s, unmatched {unmatched} s")
print("offset changes (a second, b-a offset in frames):", offs[:20])
