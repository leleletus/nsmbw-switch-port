#!/usr/bin/env python3
"""X2 (r9/r27x2): rewrite the listed GX-pipe MMIO store sites of generated files.

apply_x2.py <src_export_dir> <dst_export_dir> output_N [output_M ...]

Each `mv_.write8_io(` / `write16_io(` / `write32_io(` / `write_f32_io(` (the
stores R12 fastmem listed as MMIO: in NSMBW the write-gather pipe stores of
the GX SDK and of the game / nw4r immediate-mode drawing) becomes
`nwii::runtime::x2_w8(mv_, ` ... (runtime/x2_wgpipe.h): an in-line append to
the write-gather buffer when the address is the pipe page and the inline
paths are allowed, else the very same `mv_.writeN_io` call. Nothing else in
the file changes. Prints per file the site counts and the sha256 of input and
output (written next to the outputs as x2_sha256.txt).
"""
import hashlib, os, re, sys

REPL = [
    ('mv_.write8_io(', 'nwii::runtime::x2_w8(mv_, '),
    ('mv_.write16_io(', 'nwii::runtime::x2_w16(mv_, '),
    ('mv_.write32_io(', 'nwii::runtime::x2_w32(mv_, '),
    ('mv_.write_f32_io(', 'nwii::runtime::x2_wf32(mv_, '),
    ('nwii::runtime::psq_st_io(', 'nwii::runtime::x2_psq_st('),
]
INC_AFTER = '#include "runtime/config.h"\n'
INC = '#include "runtime/x2_wgpipe.h"  // [x2] r9/r27x2/tools/apply_x2.py\n'


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)
    log = []
    for name in sys.argv[3:]:
        s = open(os.path.join(src, name + '.cpp')).read()
        if 'x2_wgpipe.h' in s:
            sys.exit('%s: already rewritten' % name)
        hin = hashlib.sha256(s.encode()).hexdigest()
        counts = []
        for a, b in REPL:
            counts.append(s.count(a))
            s = s.replace(a, b)
        if s.count(INC_AFTER) != 1:
            sys.exit('%s: include anchor not found once' % name)
        s = '// [x2] GX pipe stores in line at the listed MMIO sites (r9/r27x2/tools/apply_x2.py)\n' + \
            s.replace(INC_AFTER, INC_AFTER + INC)
        left = len(re.findall(r'mv_\.write(8|16|32|_f32)_io\(|psq_st_io\(', s))
        if left:
            sys.exit('%s: %d sites left' % (name, left))
        out = os.path.join(dst, name + '.cpp')
        open(out, 'w').write(s)
        hout = hashlib.sha256(s.encode()).hexdigest()
        line = '%s w8=%d w16=%d w32=%d wf32=%d psq_st=%d in=%s out=%s' % (name, *counts, hin, hout)
        print(line)
        log.append(line)
    with open(os.path.join(dst, 'x2_sha256.txt'), 'a') as f:
        f.write('\n'.join(log) + '\n')


if __name__ == '__main__':
    main()
