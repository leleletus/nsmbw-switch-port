#!/usr/bin/env python3
"""fifocmp.py A.fifo B.fifo : compare two recorded GX FIFO streams (X2, r27x2).

Prints sizes, the length of the common prefix (bytes), how many XFB copies (BP 0x52 with bit 14,
written as 61 52 xx xx xx with 0x4000 set; counted on the raw bytes, so an approximation of game
frames) lie inside the identical prefix, and some context at the first difference.
"""
import sys, mmap


def frames(m, end):
    # 0x61 0x52 then 3 bytes, bit 14 of the 24-bit value (byte 3 of the word: 0x40 in the middle byte)
    n = 0; i = m.find(b'\x61\x52', 0, end)
    while i >= 0:
        if i + 5 <= end and (m[i + 3] & 0x40):
            n += 1
        i = m.find(b'\x61\x52', i + 1, end)
    return n


def main():
    a, b = sys.argv[1], sys.argv[2]
    fa, fb = open(a, 'rb'), open(b, 'rb')
    ma = mmap.mmap(fa.fileno(), 0, access=mmap.ACCESS_READ)
    mb = mmap.mmap(fb.fileno(), 0, access=mmap.ACCESS_READ)
    n = min(len(ma), len(mb))
    lo, step = 0, 1 << 20
    while lo < n:
        hi = min(n, lo + step)
        if ma[lo:hi] != mb[lo:hi]:
            for i in range(lo, hi):
                if ma[i] != mb[i]:
                    lo = i
                    break
            break
        lo = hi
    same = lo
    print('A %s: %d B\nB %s: %d B' % (a, len(ma), b, len(mb)))
    if same == n and len(ma) == len(mb):
        print('IDENTICAL: %d B, %d XFB copies' % (n, frames(ma, n)))
    else:
        print('common prefix: %d B (%.1f%% of the shorter), %d XFB copies in it%s' % (
            same, 100.0 * same / max(1, n), frames(ma, same),
            '' if same < n else ' (one is a prefix of the other)'))
        if same < n:
            c0 = max(0, same - 16)
            print('  A @%d: %s' % (c0, ma[c0:same + 16].hex(' ')))
            print('  B @%d: %s' % (c0, mb[c0:same + 16].hex(' ')))


if __name__ == '__main__':
    main()
