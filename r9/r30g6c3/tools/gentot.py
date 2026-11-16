#!/usr/bin/env python3
"""gentot.py elf:profile ... : muestras totales del hilo, en código generado (func_*, *_8XXXXXXX, _g6, envoltorios) y en idle."""
import bisect, re, subprocess, sys
NM = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm'
GEN = re.compile(r'(^func_|^run_game$|_8[0-9A-Fa-f]{7}(_g6)?$)')
for a in sys.argv[1:]:
    elf, p = a.split(':', 1)
    S = []
    for l in subprocess.run([NM, '-n', '-S', '--defined-only', elf], capture_output=True, text=True).stdout.splitlines():
        q = l.split()
        if len(q) == 4 and q[2] in 'tTwW': S.append((int(q[0], 16), int(q[1], 16), q[3]))
    st = [s[0] for s in S]; base = None; tot = gen = unk = idle = 0
    for l in open(p):
        q = l.split()
        if not q or q[0].startswith('#'): continue
        if q[0] == 'base': base = int(q[1], 16); continue
        if len(q) == 3 and q[0].isdigit():
            n = int(q[0]); off = int(q[1], 16) - base; tot += n
            i = bisect.bisect_right(st, off) - 1
            if i < 0 or off >= S[i][0] + max(S[i][1], 4): unk += n; continue
            if GEN.search(S[i][2]): gen += n
            if 'idle_wait' in S[i][2]: idle += n
    print('%-75s total %7d  gen %7d  unk %7d  idle %6d  gen/(total-unk) %.4f' % (p.split('/')[-2], tot, gen, unk, idle, gen / (tot - unk)))
