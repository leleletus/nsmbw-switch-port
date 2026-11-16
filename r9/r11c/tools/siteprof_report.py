#!/usr/bin/env python3
"""R11c: report of the fastmem site profile.

siteprof_report.py <sites.tsv> <static_sites.tsv> <out_prefix> <run_dir>...

Each run_dir holds the snapshots written by site_prof.cpp (slow_NNNN.txt,
fast_NNNN.bin, cumulative). For every run the last snapshot gives the totals;
consecutive snapshots give per-interval rates (attributed to the scene id read
at the end of the interval).

Writes <out_prefix>_sites.tsv (every site that touched non-RAM, all runs merged)
and prints the summary used in DISENO_FASTMEM.md.
"""
import collections
import glob
import os
import struct
import sys

REG = ['low16', 'page0', 'unmapped_lo', 'CP', 'PE', 'VI', 'PI', 'MI', 'DSP', 'DI/SI/EXI/AI', 'WGPIPE',
       'HW0C_other', 'HW0D', 'HW0D8', 'LC', 'unmapped']
SCENE = {0: 'BOOT', 1: 'AUTO_SELECT', 2: 'SELECT', 3: 'WORLD_MAP', 4: 'WORLD_9_DEMO', 5: 'STAGE',
         6: 'RESTART_CRSIN', 7: 'CRSIN', 8: 'MOVIE', 9: 'GAMEOVER', 10: 'GAME_SETUP'}


def load_slow(path):
    with open(path) as f:
        head = f.readline().split()
        meta = dict(x.split('=') for x in head[1:])
        rows = {}
        for l in f:
            i, r, c, amin, amax, alast = l.split()
            rows[(int(i), int(r))] = (int(c), amin, amax, alast)
    return float(meta['t']), int(meta['scene']), rows


def load_fast(path):
    d = {}
    with open(path, 'rb') as f:
        b = f.read()
    for off in range(0, len(b) - len(b) % 12, 12):
        i, c = struct.unpack_from('<IQ', b, off)
        d[i] = c
    return d


def main():
    sites_tsv, static_tsv, outp = sys.argv[1:4]
    runs = sys.argv[4:]
    site = {}
    for l in list(open(sites_tsv))[1:]:
        i, pc, op, kind, func, fn, ln = l.rstrip('\n').split('\t')
        site[int(i)] = (pc, kind, func)
    static = {}
    for l in list(open(static_tsv))[1:]:
        i, pc, kind, cls, ea = l.rstrip('\n').split('\t')
        static[int(i)] = (cls, ea)
    merged = collections.defaultdict(lambda: [0, '', '', set()])  # (id, region) -> count, amin, amax, runs
    fast_tot = collections.Counter()
    tot_t = 0.0
    print('== runs')
    for rd in runs:
        snaps = sorted(glob.glob(os.path.join(rd, 'siteprof', 'slow_*.txt')))
        if not snaps:
            print(rd, 'no snapshots'); continue
        t, sc, rows = load_slow(snaps[-1])
        fast = load_fast(snaps[-1].replace('slow_', 'fast_').replace('.txt', '.bin'))
        tot_t += t
        nf = sum(fast.values())
        ns = sum(v[0] for v in rows.values())
        name = os.path.basename(rd.rstrip('/'))
        print(f'{name}: {t:.0f} s, guest accesses {nf + ns:,} ({(nf + ns) / t / 1e6:.1f} M/s), '
              f'non-RAM {ns:,} ({ns / t:,.0f}/s), {len(fast):,} sites executed, '
              f'{len({k[0] for k in rows}):,} sites touched non-RAM')
        for k, v in rows.items():
            m = merged[k]
            m[0] += v[0]
            m[1] = v[1] if not m[1] else min(m[1], v[1])
            m[2] = max(m[2], v[2])
            m[3].add(name)
        fast_tot.update(fast)
        # per-scene rates from snapshot deltas
        prev_t, prev_rows, prev_fast = 0.0, {}, {}
        per_scene = collections.defaultdict(lambda: [0.0, collections.Counter(), 0])
        for s in snaps:
            t2, sc2, rows2 = load_slow(s)
            fast2 = load_fast(s.replace('slow_', 'fast_').replace('.txt', '.bin'))
            dt = t2 - prev_t
            ps = per_scene[SCENE.get(sc2, str(sc2))]
            ps[0] += dt
            for k, v in rows2.items():
                ps[1][REG[k[1]]] += v[0] - prev_rows.get(k, (0,))[0]
            ps[2] += sum(fast2.values()) - sum(prev_fast.values())
            prev_t, prev_rows, prev_fast = t2, rows2, fast2
        for scn, (dt, c, fa) in sorted(per_scene.items(), key=lambda x: -x[1][0]):
            top = ', '.join(f'{r} {n / dt:,.0f}/s' for r, n in c.most_common(6))
            print(f'   scene {scn:12s} {dt:5.0f} s  RAM {fa / dt / 1e6:5.1f} M/s | {top}')
    # per-region summary (merged)
    print('\n== regions (all runs merged)')
    reg_sites = collections.defaultdict(set)
    reg_cnt = collections.Counter()
    for (i, r), m in merged.items():
        reg_sites[REG[r]].add(i)
        reg_cnt[REG[r]] += m[0]
    for r, n in reg_cnt.most_common():
        st = collections.Counter(static.get(i, ('?',))[0] for i in reg_sites[r])
        print(f'{r:14s} sites {len(reg_sites[r]):5d}  accesses {n:14,} ({n / tot_t:12,.0f}/s)  static: {dict(st)}')
    # per site
    per_site = collections.defaultdict(lambda: [0, set(), '', ''])
    for (i, r), m in merged.items():
        p = per_site[i]
        p[0] += m[0]
        p[1].add(REG[r])
        p[2] = m[1] if not p[2] else min(p[2], m[1])
        p[3] = max(p[3], m[2])
    with open(outp + '_sites.tsv', 'w') as o:
        o.write('id\tpc\tkind\tfunc\tregions\tnonram\tnonram_per_s\tram\tstatic\tstatic_ea\tamin\tamax\n')
        for i, p in sorted(per_site.items(), key=lambda x: -x[1][0]):
            pc, kind, func = site[i]
            cls, ea = static.get(i, ('?', ''))
            o.write(f'{i}\t{pc}\t{kind}\t{func}\t{",".join(sorted(p[1]))}\t{p[0]}\t{p[0] / tot_t:.1f}\t'
                    f'{fast_tot.get(i, 0)}\t{cls}\t{ea}\t{p[2]}\t{p[3]}\n')
    # classification coverage
    print('\n== classification of the sites that would FAULT with fastmem (MMIO/pipe/unmapped; LC is mapped,'
          ' page0/low16 apart)')
    io = {i: p for i, p in per_site.items() if p[1] - {'page0', 'low16', 'LC'}}
    # their non-RAM count without the mapped regions
    io_cnt = {}
    for (i, r), m in merged.items():
        if i in io and REG[r] not in ('page0', 'low16', 'LC'):
            io_cnt[i] = io_cnt.get(i, 0) + m[0]
    for i in io:
        io[i] = [io_cnt[i]] + io[i][1:]
    stat_io = {i for i in io if static.get(i, ('unknown',))[0] not in ('unknown', 'RAM', 'page0', 'low16')}
    mixed = {i for i in io if fast_tot.get(i, 0) > 0 or (io[i][1] & {'LC', 'page0', 'low16'})}
    pure = [i for i in io if i not in mixed]
    print(f'pure MMIO sites (only ever non-RAM): {len(pure)}, {sum(io[i][0] for i in pure) / tot_t:,.0f}/s, '
          f'{sum(1 for i in pure if i in stat_io)} of them static; mixed sites: {len(mixed)}, '
          f'{sum(io[i][0] for i in mixed) / tot_t:,.0f}/s non-RAM, {sum(1 for i in mixed if i in stat_io)} static')
    mreg = collections.Counter(r for i in mixed for r in io[i][1])
    print(f'  regions of the mixed sites: {dict(mreg)}')
    tot = sum(p[0] for p in io.values())
    cov = sum(io[i][0] for i in stat_io)
    print(f'sites: {len(io)}; statically MMIO (lis/addi/ori const-prop): {len(stat_io)} '
          f'({100 * len(stat_io) / max(1, len(io)):.1f} % of sites, {100 * cov / max(1, tot):.2f} % of accesses)')
    for th in (1, 10, 100, 1000, 10000):
        hot = [i for i in io if io[i][0] / tot_t >= th]
        nst = [i for i in hot if i not in stat_io]
        print(f'  >= {th:5d}/s: {len(hot):4d} sites, {len(nst):4d} of them NOT static '
              f'({sum(io[i][0] for i in nst) / tot_t:,.0f}/s would fault without the list)')
    all_static_io = [i for i, (c, e) in static.items() if c not in ('unknown', 'RAM', 'page0', 'low16')]
    print(f'static MMIO sites in the whole program: {len(all_static_io)}, executed in these runs: '
          f'{sum(1 for i in all_static_io if i in io)}')
    print(f'\nguest RAM accesses (fast): {sum(fast_tot.values()) / tot_t / 1e6:.1f} M/s over {tot_t:.0f} s')
    pg = {i: p for i, p in per_site.items() if p[1] & {'page0', 'low16'}}
    pgw = collections.Counter()
    for (i, r), m in merged.items():
        if REG[r] in ('page0', 'low16'):
            pgw['write' if site[i][1].startswith('write') or site[i][1] == 'psq_st' else 'read'] += m[0]
    print(f'page0/low16 reads {pgw["read"] / tot_t:,.0f}/s, writes {pgw["write"] / tot_t:,.0f}/s')
    print(f'page0/low16 sites: {len(pg)}, accesses {sum(p[0] for p in pg.values()) / tot_t:,.0f}/s; '
          f'low16 sites: {sum(1 for p in pg.values() if "low16" in p[1])}')




def write_list(sites_tsv, static_tsv, prof_sites_tsv, out):
    """MMIO list for the recompiler: static const-prop MMIO sites + profiled sites that hit
    MMIO/pipe/unmapped (LC and page 0 are mapped by fastmem). Keyed by guest pc + kind."""
    keep = {}
    for l in list(open(static_tsv))[1:]:
        i, pc, kind, cls, ea = l.rstrip('\n').split('\t')
        if cls not in ('unknown', 'RAM', 'page0', 'low16'):
            keep[(pc, kind)] = 'static:' + cls
    for l in list(open(prof_sites_tsv))[1:]:
        f = l.rstrip('\n').split('\t')
        regs = set(f[4].split(',')) - {'LC', 'page0', 'low16'}
        if regs and (f[1], f[2]) not in keep:
            keep[(f[1], f[2])] = 'profile:' + ','.join(sorted(regs))
    with open(out, 'w') as o:
        o.write('pc\tkind\tsource\n')
        for (pc, kind), src in sorted(keep.items()):
            o.write(f'{pc}\t{kind}\t{src}\n')
    print(f'MMIO list: {len(keep)} (pc, kind) entries -> {out} '
          f'({sum(1 for v in keep.values() if v.startswith("profile"))} only from the profile)')


if __name__ == '__main__':
    main()
    write_list(sys.argv[1], sys.argv[2], sys.argv[3] + '_sites.tsv', sys.argv[3] + '_mmio_list.tsv')
