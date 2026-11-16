#!/usr/bin/env python3
"""fnsamples.py PROFILE@ELF ... > fn_weights.tsv
Per generated function (by name, so profiles of different builds add up): the share of the generated-code
samples of each profile, averaged over the profiles (each profile weighs the same), and the raw samples.
Output: name<TAB>avg_share(ppm)<TAB>samples per profile..."""
import bisect
import collections
import subprocess
import sys

NM = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm'


def gen(n):
    import re
    return n.startswith('func_') or re.search(r'_8[0-9A-Fa-f]{7}$', n) is not None


def one(prof, elf):
    base = None
    pairs = []
    for l in open(prof):
        p = l.split()
        if not p or p[0].startswith('#'):
            continue
        if p[0] == 'base':
            base = int(p[1], 16)
        elif len(p) == 3 and p[0].isdigit():
            pairs.append((int(p[0]), int(p[1], 16) - base))
    syms = []
    for l in subprocess.run([NM, '-n', '-S', '--defined-only', elf], capture_output=True, text=True).stdout.splitlines():
        q = l.split()
        if len(q) == 4 and q[2] in 'tTwW':
            syms.append((int(q[0], 16), int(q[1], 16), q[3]))
    st = [s[0] for s in syms]
    c = collections.Counter()
    for n, o in pairs:
        i = bisect.bisect_right(st, o) - 1
        if i >= 0 and o < syms[i][0] + max(4, syms[i][1]) and gen(syms[i][2]):
            c[syms[i][2]] += n
    return c


def main():
    per = []
    for a in sys.argv[1:]:
        prof, elf = a.split('@')
        per.append(one(prof, elf))
    names = set().union(*per)
    tot = [max(1, sum(c.values())) for c in per]
    rows = []
    for n in names:
        share = sum(c.get(n, 0) / t for c, t in zip(per, tot)) / len(per)
        rows.append((share, n))
    rows.sort(reverse=True)
    for share, n in rows:
        print('%s\t%.1f\t%s' % (n, share * 1e6, '\t'.join(str(c.get(n, 0)) for c in per)))
    sys.stderr.write('profiles: %s generated samples\n' % tot)


if __name__ == '__main__':
    main()
