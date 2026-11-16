#!/usr/bin/env python3
"""R11c (fastmem groundwork) -- PROFILING ONLY, never for a shipped build.

apply_site_ids.py <export_dir> <sites.tsv> [--orig <export_dir_original>]

Gives every guest memory-access site of the generated code a dense site id and
its guest instruction address (the preceding `// 0xADDR: OPCODE` comment), so a
profiling MemView (NWII_SITE_PROF in r9/r11c/nWiiRuntime cpu_context.h) can
count, per site, the accesses that hit plain RAM and log the ones that do not
(MMIO, gather pipe, locked cache, page 0, unmapped).

  mv_.read32(ea)               -> mv_.read32_s(ID, ea)
  mv_.write_f32(ea, v)         -> mv_.write_f32_s(ID, ea, v)
  psq_l_fast(ctx, mv_, ...)    -> psq_l_fast_s(ID, ctx, mv_, ...)
  psq_st_fast(ctx, mv_, ...)   -> psq_st_fast_s(ID, ctx, mv_, ...)

The *_s variants count and then call the unchanged accessor, so the profiled
binary reads and writes exactly the same bytes in the same order.

Safety (as the r9 perf passes): each output_*.cpp is marked `// [r11c-siteid]`
on its first line and is skipped if already marked (idempotent); with --orig
the sha256 of every input file must equal the original export's file (the
pass refuses to run on anything else). Sites inside code without `// 0x`
comments (r9 axfx native body) get a synthetic pc 0xAF00xxxx and the function
name, so they are still counted.

sites.tsv columns: id pc opcode kind func file line
"""
import hashlib
import os
import re
import sys

MARK = '// [r11c-siteid]'
RX_FUNC = re.compile(r'^(?:static\s+)?(?:NWII_NOINLINE\s+)?void\s+(\w+)\(CPUContext\s*&\s*ctx\)\s*\{')
RX_PC = re.compile(r'^\s*// 0x([0-9A-Fa-f]{8}): ([0-9A-Fa-f]{8})\s*$')
RX_MV = re.compile(r'\bmv_\.(read8|read16|read32|read64|read_f32|read_f64|'
                   r'write8|write16|write32|write64|write_f32|write_f64)\(')
RX_PSQ = re.compile(r'\bpsq_(l|st)_fast\(ctx, mv_,')


def sha(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        h.update(f.read())
    return h.hexdigest()


def main():
    args = sys.argv[1:]
    orig = None
    if '--orig' in args:
        i = args.index('--orig')
        orig = args[i + 1]
        del args[i:i + 2]
    exp, tsv = args
    files = sorted((f for f in os.listdir(exp) if re.fullmatch(r'output_\d+\.cpp', f)),
                   key=lambda f: int(f[7:-4]))
    rows = []
    nid = 0
    synth = 0
    for fn in files:
        path = os.path.join(exp, fn)
        with open(path) as f:
            text = f.read()
        if text.startswith(MARK):
            print(f'{fn}: already marked, skipped (re-run on a fresh copy to rebuild sites.tsv)')
            sys.exit(2)
        if orig:
            a, b = sha(path), sha(os.path.join(orig, fn))
            if a != b:
                print(f'{fn}: sha256 differs from the original export ({a} != {b}); refusing')
                sys.exit(3)
        out = []
        func = '?'
        pc = None
        op = None
        for ln, line in enumerate(text.split('\n'), 1):
            m = RX_FUNC.match(line)
            if m:
                func = m.group(1)
                pc = None
                op = None
            m = RX_PC.match(line)
            if m:
                pc = int(m.group(1), 16)
                op = m.group(2).upper()

            def site_pc():
                nonlocal synth
                if pc is not None:
                    return pc, op
                synth += 1
                return 0xAF000000 | synth, '-'

            def rep_mv(m):
                nonlocal nid
                p, o = site_pc()
                rows.append((nid, p, o, m.group(1), func, fn, ln))
                s = f'mv_.{m.group(1)}_s({nid}u, '
                nid += 1
                return s

            def rep_psq(m):
                nonlocal nid
                p, o = site_pc()
                kind = 'psq_l' if m.group(1) == 'l' else 'psq_st'
                rows.append((nid, p, o, kind, func, fn, ln))
                s = f'psq_{m.group(1)}_fast_s({nid}u, ctx, mv_,'
                nid += 1
                return s

            if 'mv_.' in line and not line.lstrip().startswith('nwii::runtime::MemView'):
                line = RX_MV.sub(rep_mv, line)
            if '_fast(ctx, mv_,' in line:
                line = RX_PSQ.sub(rep_psq, line)
            out.append(line)
        with open(path, 'w') as f:
            f.write(MARK + ' site ids for the fastmem profile (r9/r11c/tools/apply_site_ids.py)\n')
            f.write('\n'.join(out))
    with open(tsv, 'w') as f:
        f.write('id\tpc\topcode\tkind\tfunc\tfile\tline\n')
        for r in rows:
            f.write(f'{r[0]}\t{r[1]:08X}\t{r[2]}\t{r[3]}\t{r[4]}\t{r[5]}\t{r[6]}\n')
    if nid >= (1 << 20):
        print('more than 2^20 sites: grow kSiteMax in site_prof.cpp / cpu_context.h'); sys.exit(4)
    print(f'{len(files)} files, {nid} sites, {synth} without a // 0x comment -> {tsv}')


if __name__ == '__main__':
    main()
