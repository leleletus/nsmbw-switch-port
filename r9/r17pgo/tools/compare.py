#!/usr/bin/env python3
"""compare.py <bench dir> [<bench dir> ...]: game-thread time per frame from bench_title.sh runs.
Each "[NX] game X fps ... threads main a%, cpu b% ..." line of nwii.log is a 5 s window; the game thread
needs b/100 * 1000/X ms of CPU per frame (busy time; waits excluded). Prints per run the windows side by side
(same index = same moment of the title demo, the runs are aligned at the title) and summaries:
all windows, the castle cutscene stretch and the last 'desierto' stretch (heaviest windows).
The first column is the reference."""
import re, sys, os

RX = re.compile(r'\[NX\] game ([0-9.]+) fps.*?cpu ([0-9]+) MHz.*?threads main ([0-9]+)%, cpu ([0-9]+)%')

def load(d):
    rows = []
    for line in open(os.path.join(d, 'nwii.log'), errors='replace'):
        m = RX.search(line)
        if m:
            fps, mhz, main, cpu = float(m[1]), int(m[2]), int(m[3]), int(m[4])
            rows.append((fps, cpu, cpu / 100 * 1000 / fps if fps > 1 else 0, main))
    return rows[4:]  # same skip as bench_title.sh (boot)

runs = [(os.path.basename(d.rstrip('/')), load(d)) for d in sys.argv[1:]]
n = max(len(r) for _, r in runs)
print('win ' + ''.join(f'{name[:22]:>24}' for name, _ in runs))
for i in range(n):
    cells = []
    for _, r in runs:
        cells.append(f'{r[i][0]:5.1f}fps {r[i][2]:5.1f}ms' if i < len(r) else '')
    print(f'{i:3d} ' + ''.join(f'{c:>24}' for c in cells))

def stats(r, sel):
    v = [x for i, x in enumerate(r) if sel(i, x) and x[0] > 5]
    if not v:
        return '-'
    return f'{sum(x[0] for x in v)/len(v):5.1f}fps {sum(x[2] for x in v)/len(v):5.2f}ms (n={len(v)})'

print()
m = min(len(r) for _, r in runs)
ref = runs[0][1]
heavy = {i for i in range(m) if ref[i][2] >= 15}
for label, sel in [(f'0..{m-1}', lambda i, x: i < m),
                   ('pesadas(ref>=15ms)', lambda i, x: i in heavy),
                   ('ultimas 7', lambda i, x: m - 7 <= i < m)]:
    print(f'{label:20s}' + ''.join(f'{stats(r, sel):>30}' for _, r in runs))
