#!/usr/bin/env python3
"""check_fm_tab.py <x86-64 ELF> [sites.tsv]
Every entry of section nwii_fm_tab ({int32 pc - &entry, uint32 kind | id << 8}) must point at an
instruction of the fixed fastmem form: mov/movzx load or mov store, [base + index] (SIB, scale 1,
no displacement unless the base is rbp/r13 with disp8 0), width/direction matching the kind.
With sites.tsv also checks that every fastmem site id has at least one entry."""
import struct, sys
from collections import Counter

p = sys.argv[1]
d = open(p, 'rb').read()
assert d[:4] == b'\x7fELF' and d[4] == 2
e_shoff, = struct.unpack_from('<Q', d, 0x28)
e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', d, 0x3A)
secs = []
for i in range(e_shnum):
    o = e_shoff + i * e_shentsize
    name, typ, flags, addr, off, size = struct.unpack_from('<IIQQQQ', d, o)
    secs.append([name, typ, flags, addr, off, size])
so = secs[e_shstrndx][4]
def nm(n):
    return d[so + n:d.index(b'\0', so + n)].decode()
for s in secs:
    s[0] = nm(s[0])
tab = [s for s in secs if s[0] == 'nwii_fm_tab']
if not tab:
    print('no nwii_fm_tab section'); sys.exit(1)
_, _, _, taddr, toff, tsize = tab[0]
code = [s for s in secs if s[2] & 4 and s[1] == 1]  # SHF_EXECINSTR, PROGBITS
def byts(va, n=16):
    for s in code:
        if s[3] <= va < s[3] + s[5]:
            o = s[4] + (va - s[3])
            return d[o:o + n]
    return None

SIZE = {1: 1, 2: 2, 3: 4, 4: 8, 5: 4, 6: 8, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8}
def decode(b):
    i = 0; opsize = False; rex = 0
    while b[i] in (0x66, 0x2E, 0x3E):
        opsize |= b[i] == 0x66; i += 1
    if b[i] & 0xF0 == 0x40:
        rex = b[i]; i += 1
    W = bool(rex & 8)
    op = b[i]; i += 1
    if op == 0x0F:
        op2 = b[i]; i += 1
        if op2 == 0xB6: st, sz = False, 1
        elif op2 == 0xB7: st, sz = False, 2
        else: return None
    elif op == 0x8B: st, sz = False, 8 if W else 2 if opsize else 4
    elif op == 0x8A: st, sz = False, 1
    elif op == 0x89: st, sz = True, 8 if W else 2 if opsize else 4
    elif op == 0x88: st, sz = True, 1
    else: return None
    modrm = b[i]; i += 1
    mod, rm = modrm >> 6, modrm & 7
    if mod == 3 or rm != 4: return ('form', st, sz)
    sib = b[i]; i += 1
    if sib >> 6 != 0 or ((sib >> 3) & 7) == 4 and not (rex & 2): return ('form', st, sz)
    if mod == 1:
        if b[i] != 0: return ('form', st, sz)
    elif mod != 0: return ('form', st, sz)
    if mod == 0 and (sib & 7) == 5: return ('form', st, sz)
    return ('ok', st, sz)

machine, = struct.unpack_from('<H', d, 0x12)
def decode_a64(b):
    w, = struct.unpack_from('<I', b, 0)
    # LDR/STR (register offset), integer, option UXTW (010), S = 0: size 111 0 00 opc 1 Rm 010 0 10 Rn Rt
    if (w & 0x3F20FC00) != 0x38204800: return None
    size, opc = w >> 30, (w >> 22) & 3
    if opc > 1: return ('form', None, None)
    return ('ok', opc == 0, 1 << size)
if machine == 0xB7:
    decode = decode_a64
n = tsize // 8
bad = Counter(); ids = set(); kinds = Counter()
examples = []
for k in range(n):
    rel, info = struct.unpack_from('<iI', d, toff + 8 * k)
    pc = taddr + 8 * k + rel
    kind, sid = info & 0xFF, info >> 8
    ids.add(sid); kinds[kind] += 1
    b = byts(pc)
    r = decode(b) if b else None
    why = None
    if r is None: why = 'not a mov'
    elif r[0] != 'ok': why = 'address form'
    elif r[1] != (kind >= 7): why = 'direction'
    elif r[2] != SIZE.get(kind): why = 'width'
    if why:
        bad[why] += 1
        if len(examples) < 10: examples.append((hex(pc), kind, sid, why, b[:8].hex() if b else None))
print(f'{n} entries, {len(ids)} site ids, kinds {dict(sorted(kinds.items()))}')
print('bad:', dict(bad) or 0)
for e in examples: print('  ', e)
if len(sys.argv) > 2:
    want = set()
    for ln in open(sys.argv[2]):
        f = ln.split('\t')
        if f[0].isdigit(): want.add(int(f[0]))
    miss = want - ids
    print(f'sites.tsv: {len(want)} fastmem ids, {len(miss)} without a table entry'
          + (f' (e.g. {sorted(miss)[:10]})' if miss else ''))
sys.exit(1 if bad else 0)
