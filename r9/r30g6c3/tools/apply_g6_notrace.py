#!/usr/bin/env python3
"""apply_g6_notrace.py <export_dir> --orig <same export, untouched>
G6: drops the call-trace test every generated function starts with
    if (nwii::runtime::g_trace_calls && ctx.pc == 0xF) nwii::runtime::trace_call(0xF, ctx);
(a global load + test + branch per guest call). It only prints a line when NWII_TRACE_CALLS=1 (debugging),
so the guest state is the same; with this pass that variable has no effect. Marks files `// [r20-g6t]`."""
import hashlib
import os
import re
import sys

MARK = '// [r20-g6t]'
TRACE = re.compile(r'^    if \(nwii::runtime::g_trace_calls && ctx\.pc == 0x[0-9A-F]+\) nwii::runtime::trace_call\(0x[0-9A-F]+, ctx\);$')


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def main():
    d = sys.argv[1]
    orig = sys.argv[sys.argv.index('--orig') + 1]
    n = 0
    for x in sorted(os.listdir(d)):
        if not re.match(r'output_\d+\.cpp$', x):
            continue
        p = os.path.join(d, x)
        if open(p).readline().startswith(MARK):
            continue
        if sha(p) != sha(os.path.join(orig, x)):
            sys.exit(f'{x}: sha256 differs from {orig}; refusing')
        src = open(p).read().split('\n')
        out = [MARK + ' call-trace test removed (r9/r20g6/tools/apply_g6_notrace.py)']
        for l in src:
            if TRACE.match(l):
                n += 1
                continue
            if 'g_trace_calls' in l:
                sys.exit(f'{x}: unknown trace form: {l[:80]}')
            out.append(l)
        open(p + '.tmp', 'w').write('\n'.join(out))
        os.replace(p + '.tmp', p)
    print('g6t: removed %d trace tests' % n)


if __name__ == '__main__':
    main()
