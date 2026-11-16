#!/usr/bin/env python3
"""static_cmp.py <label=nx_obj_dir>... [--top N] [--weights fn_weights.tsv]

Static AArch64 size of the generated code (symbol sizes of output_*.o, devkitA64 nm), per build:
  - total instructions of all generated functions;
  - weighted by the console samples per function (fn_weights.tsv, r20g6/tools/fnsamples.py):
    sum(w * instructions) / sum(w) -- a proxy for "how much code the hot functions are";
  - the N hottest functions side by side.
A function's cost is its _g6 body when it has one (G6 stage 3: the wrapper is only used by run_game /
nwii_dispatch), else the function itself. Instructions = bytes / 4.
"""
import os, subprocess, sys

NM = '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm'


def sizes(d):
    out = {}
    objs = sorted(os.path.join(d, f) for f in os.listdir(d) if f.startswith('output_') and f.endswith('.o'))
    for o in objs:
        r = subprocess.run([NM, '-S', '--defined-only', o], capture_output=True, text=True, check=True)
        for l in r.stdout.split('\n'):
            p = l.split()
            if len(p) == 4 and p[2] in 'Tt':
                out[p[3]] = out.get(p[3], 0) + int(p[1], 16) // 4
    return out


def cost(sz, name):
    if name + '_g6' in sz:
        return sz[name + '_g6']
    return sz.get(name)


def main():
    a = sys.argv[1:]
    top = 25
    wf = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'fn_weights.tsv')
    if '--top' in a:
        k = a.index('--top'); top = int(a[k + 1]); del a[k:k + 2]
    if '--weights' in a:
        k = a.index('--weights'); wf = a[k + 1]; del a[k:k + 2]
    w = {}
    for l in open(wf):
        p = l.split()
        if len(p) >= 2:
            w[p[0]] = float(p[1])
    builds = []
    for x in a:
        lab, d = x.split('=', 1)
        builds.append((lab, sizes(d)))
    names = [n for n in w if all(cost(s, n) is not None for _, s in builds)]
    W = sum(w[n] for n in names)
    print('functions with samples present in every build: %d (weight %.0f)' % (len(names), W))
    base = None
    for lab, s in builds:
        tot = sum(v for k, v in s.items() if not k.startswith('_Z'))
        ws = sum(w[n] * cost(s, n) for n in names) / W
        if base is None:
            base = (tot, ws)
        print('%-10s total %10d ins (%+.1f %%)   weighted %9.1f ins/fn (%+.1f %%)' %
              (lab, tot, 100.0 * (tot / base[0] - 1), ws, 100.0 * (ws / base[1] - 1)))
    hot = sorted(names, key=lambda n: -w[n])[:top]
    print('%-40s %8s ' % ('function', 'weight') + ' '.join('%9s' % lab for lab, _ in builds))
    for n in hot:
        print('%-40s %8.0f ' % (n[:40], w[n]) + ' '.join('%9d' % cost(s, n) for _, s in builds))


if __name__ == '__main__':
    main()
