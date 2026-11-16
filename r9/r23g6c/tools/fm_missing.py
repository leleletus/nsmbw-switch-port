#!/usr/bin/env python3
"""fm_missing.py <elf> <sites.tsv>: fastmem site ids with no nwii_fm_tab entry (GCC dropped the access)."""
import struct, sys
d = open(sys.argv[1], 'rb').read()
e_shoff, = struct.unpack_from('<Q', d, 0x28)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x3A)
secs = []
for i in range(e_shnum):
    o = e_shoff + i * e_shentsize
    secs.append(list(struct.unpack_from('<IIQQQQ', d, o)))
so = secs[e_shstrndx][4]
nm = lambda n: d[so + n:d.index(b'\0', so + n)].decode()
t = [s for s in secs if nm(s[0]) == 'nwii_fm_tab'][0]
ids = set()
for k in range(t[5] // 8):
    _, v = struct.unpack_from('<iI', d, t[4] + 8 * k)
    ids.add(v >> 8)
for l in open(sys.argv[2]):
    p = l.split('\t')
    if p[0].isdigit() and int(p[0]) not in ids:
        print(l.rstrip())
