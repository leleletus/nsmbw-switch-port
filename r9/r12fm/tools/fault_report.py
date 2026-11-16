#!/usr/bin/env python3
"""fault_report.py <sites.tsv> <run.log>...: last [FASTMEM] dump of each log, joined with the site table
(guest pc, kind, function) -> one line per faulting site: faults, rate, region of the last ea."""
import sys, re
sites = {}
for ln in open(sys.argv[1]):
    f = ln.rstrip('\n').split('\t')
    if f[0].isdigit():
        sites[int(f[0])] = f
def region(ea):
    p = ea & 0x3FFFFFFF
    if p < 0x01800000: return 'MEM1'
    if 0x0C008000 <= p < 0x0C009000: return 'WGPIPE'
    if 0x0C000000 <= p < 0x0C010000: return 'HW 0C00'
    if 0x0D000000 <= p < 0x0D010000: return 'HW 0D00'
    if ea >= 0xE0000000: return 'LC/E'
    if 0x10000000 <= p < 0x14000000: return 'MEM2'
    return 'unmapped'
for log in sys.argv[2:]:
    lines = open(log, errors='replace').read().split('\n')
    last = max((i for i, l in enumerate(lines) if l.startswith('[FASTMEM] dump')), default=None)
    if last is None:
        print(f'{log}: no [FASTMEM] dump'); continue
    print(f'{log}: {lines[last]}')
    for l in lines[last + 1:]:
        m = re.match(r'\[FASTMEM\] site (\d+) kind (\d+) faults (\d+) last_ea ([0-9A-F]+)', l)
        if not m: continue
        sid, n, ea = int(m.group(1)), int(m.group(3)), int(m.group(4), 16)
        s = sites.get(sid, ['?', '?', '?', '?', '?', '?'])
        print(f'  site {sid:7d} pc {s[1]} {s[2]:9s} {s[3]:28s} faults {n:9d} last_ea {ea:08X} {region(ea)}')
