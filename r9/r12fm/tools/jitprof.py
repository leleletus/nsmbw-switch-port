#!/usr/bin/env python3
"""jitprof.py <run_dir> <nsmbw.elf> [top=40]

Profile of the guest (Switch NRO) code inside citron, from a citron_perf.sh run:
  <run_dir>/perf.data    perf record of citron's CPUCore_1 / CPUCore_0 threads
  <run_dir>/pm/perf-<pid>.map   dynarmic's JIT block map (host start, size, name with the guest PC)
Each sample's host IP is resolved to its JIT block, the block's guest PC to the
NRO function (symbols of <nsmbw.elf>, which must be the ELF of the NRO that ran).
The NRO load base is found automatically (the page-aligned base that puts the most
block entries exactly at function starts).
Output: per thread, share of samples in JIT code / citron itself, then the top
guest functions (% of that thread's samples).
"""
import bisect, collections, glob, os, re, subprocess, sys


def load_map(path):
    starts, ends, pcs = [], [], []
    rows = []
    with open(path, errors='replace') as f:
        for line in f:
            p = line.split(None, 2)
            if len(p) < 3:
                continue
            try:
                s = int(p[0], 16); n = int(p[1], 16)
            except ValueError:
                continue
            m = re.search(r'([0-9A-Fa-f]{8,16})', p[2])
            if not m:
                continue
            rows.append((s, n, int(m.group(1), 16)))
    rows.sort()
    for s, n, pc in rows:
        starts.append(s); ends.append(s + n); pcs.append(pc)
    return starts, ends, pcs


def load_syms(elf):
    nm = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm'
    out = subprocess.run([nm, '-C', '--defined-only', '-n', elf], capture_output=True, text=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        p = line.split(' ', 2)
        if len(p) == 3 and p[1] in 'tTwW':
            addrs.append(int(p[0], 16)); names.append(p[2])
    return addrs, names


def main():
    run, elf = sys.argv[1], sys.argv[2]
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    maps = glob.glob(os.path.join(run, 'pm', 'perf-*.map'))
    if not maps:
        sys.exit('no pm/perf-*.map in ' + run)
    mp = max(maps, key=os.path.getsize)
    starts, ends, pcs = load_map(mp)
    addrs, names = load_syms(elf)
    aset = set(addrs)
    # base: most block PCs on a function start
    cnt = collections.Counter()
    sample_pcs = pcs[:: max(1, len(pcs) // 20000)]
    for pc in sample_pcs[:4000]:
        for a in addrs[:: max(1, len(addrs) // 3000)]:
            b = pc - a
            if b >= 0 and b & 0xFFF == 0:
                cnt[b] += 1
    best, bestn = None, -1
    for b, _ in cnt.most_common(30):
        n = sum(1 for pc in sample_pcs if (pc - b) in aset)
        if n > bestn:
            best, bestn = b, n
    print('# map %s: %d blocks; NRO base 0x%x (%d of %d sampled blocks start a function)' % (
        os.path.basename(mp), len(pcs), best, bestn, len(sample_pcs)))
    ps = subprocess.run(['perf', 'script', '-i', os.path.join(run, 'perf.data'), '-F', 'tid,ip'],
                        capture_output=True, text=True).stdout
    per = collections.defaultdict(collections.Counter)
    tot = collections.Counter()
    for line in ps.splitlines():
        p = line.split()
        if len(p) < 2:
            continue
        tid = p[0]
        try:
            ip = int(p[1], 16)
        except ValueError:
            continue
        tot[tid] += 1
        i = bisect.bisect_right(starts, ip) - 1
        if i >= 0 and ip < ends[i]:
            g = pcs[i] - best
            j = bisect.bisect_right(addrs, g) - 1
            name = names[j] if j >= 0 else '?'
            per[tid][name] += 1
        else:
            per[tid]['[host: citron / outside JIT]'] += 1
    comm = {}
    pl = os.path.join(run, 'perf.log')
    if os.path.exists(pl):
        m = re.search(r'CPUCore_1=(\d+) CPUCore_0=(\d+)', open(pl).read())
        if m:
            comm = {m.group(1): 'CPUCore_1 (game CPU thread)', m.group(2): 'CPUCore_0 (main/render)'}
    for tid, c in sorted(per.items(), key=lambda kv: -tot[kv[0]]):
        n = tot[tid]
        print('\n== tid %s %s: %d samples' % (tid, comm.get(tid, ''), n))
        for name, k in c.most_common(top):
            print('   %5.1f%%  %s' % (100.0 * k / n, name[:110]))


if __name__ == '__main__':
    main()
