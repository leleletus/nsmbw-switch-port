#!/usr/bin/env python3
"""apply_r9_psq.py <export_dir>

R9 cpucode post-pass (after apply_r9_cpucode.py; independent of apply_r9_axfx.py).
Every psq_l/psq_lu/psq_lx/psq_st... of the generated code was a call to the
out-of-line CPUContext::psq_load/psq_store (GQR lookup, switch on the type,
MMU read/write; ~4 % of the CPU thread in the PC profile, matrix/vector code of
the SDK and the game). This pass rewrites each

    ctx.psq_load(D, EA, W, I);   ->  nwii::runtime::psq_l_fast(ctx, mv_, D, EA, W, I);
    ctx.psq_store(S, EA, W, I);  ->  nwii::runtime::psq_st_fast(ctx, mv_, S, EA, W, I);

psq_l_fast/psq_st_fast (cpu_context.h) do the float type in MEM1 inline (same
test as MemView, same order, same conversions) and call the original function
for everything else, before touching anything. Same values, same memory, same
order. Every function that has a psq op already has its MemView `mv_`.
Applied once: marks every rewritten output_*.cpp with // [r9psq].
"""
import os, re, sys

MARK = '// [r9psq]'
PAT = re.compile(r'ctx\.psq_(load|store)\((\d+), ([^,;()]+(?:\([^()]*\))?[^,;()]*), ([01]), ([0-7])\);')


def main():
    d = sys.argv[1]
    files = sorted(f for f in os.listdir(d) if re.fullmatch(r'output_\d+\.cpp', f))
    total = {'load': 0, 'store': 0}
    for f in files:
        p = os.path.join(d, f)
        s = open(p).read()
        if MARK in s:
            sys.exit('apply_r9_psq: already applied to ' + p)
        n = {'load': 0, 'store': 0}

        def rep(m):
            n[m.group(1)] += 1
            fn = 'psq_l_fast' if m.group(1) == 'load' else 'psq_st_fast'
            return 'nwii::runtime::%s(ctx, mv_, %s, %s, %s, %s);' % (fn, m.group(2), m.group(3), m.group(4), m.group(5))

        s2 = PAT.sub(rep, s)
        left = s2.count('ctx.psq_load(') + s2.count('ctx.psq_store(')
        if left:
            sys.exit('apply_r9_psq: %s: %d psq calls not matched' % (p, left))
        if n['load'] or n['store']:
            # every function containing a rewritten call needs its MemView (functions with
            # psq ops and no other memory access had none: give them one, as the others have)
            added = []

            def fix(fm):
                body = fm.group(3)
                if ('psq_l_fast(ctx, mv_' in body or 'psq_st_fast(ctx, mv_' in body) and \
                        'nwii::runtime::MemView mv_(ctx.mmu);' not in body:
                    added.append(fm.group(2))
                    return fm.group(1) + '    nwii::runtime::MemView mv_(ctx.mmu);  ' + MARK + '\n' + body + '}\n'
                return fm.group(0)

            s2 = re.sub(r'^(void (\w+)\(CPUContext& ctx\) \{\n)(.*?)^\}\n', fix, s2, flags=re.M | re.S)
            if added:
                print('apply_r9_psq: %s: MemView added to %s' % (f, ' '.join(added)))
            s2 = MARK + ' psq_l/psq_st inline fast path (perf/apply_r9_psq.py): %d loads, %d stores\n' % (
                n['load'], n['store']) + s2
            open(p, 'w').write(s2)
        total['load'] += n['load']
        total['store'] += n['store']
    print('apply_r9_psq: %d psq_load, %d psq_store rewritten' % (total['load'], total['store']))


if __name__ == '__main__':
    main()
