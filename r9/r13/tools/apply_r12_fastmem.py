#!/usr/bin/env python3
"""R12 fastmem post-pass over the generated code (copy of r9/r10c/export).

apply_r12_fastmem.py <export_dir> <mmio_list.tsv> <sites_out.tsv> --orig <r10c export dir>

Every guest access of the generated code gets a dense site id (kept in the exception table of
the fastmem accessor, so a fault can be traced back to its guest pc through sites_out.tsv):
  mv_.read32(ea)              -> mv_.read32<ID>(ea)            (fastmem: asm access + table entry)
  psq_l_fast(ctx, mv_, ...)   -> psq_l_fast<ID>(ctx, mv_, ...)
Sites whose guest pc is in the MMIO list (r9/r11c/prof_mmio_list.tsv: static classifier +
profile) call the MMU slow path directly instead (never fault):
  mv_.write32(ea, v)          -> mv_.write32_io(ea, v)
  psq_st_fast(ctx, mv_, ...)  -> psq_st_io(ctx, mv_, ...)
Plus r9_axfx_hash(const std::vector<uint8_t>&) -> (const nwii::runtime::MemBlock&) (MMU memory
is no longer a std::vector).

Safety (as the r9 perf passes): every output_*.cpp is marked `// [r12-fastmem]` on its first
line; a marked file is skipped (idempotent); unmarked files must have the sha256 of the same
file in --orig, or the pass refuses to touch them.
"""
import hashlib, os, re, sys

MARK = '// [r12-fastmem]'
RX_FUNC = re.compile(r'^(?:static\s+)?(?:NWII_NOINLINE\s+)?void\s+(\w+)\(CPUContext\s*&\s*ctx\)\s*\{')
RX_PC = re.compile(r'^\s*// 0x([0-9A-Fa-f]{8}): ([0-9A-Fa-f]{8})\s*$')
RX_MV = re.compile(r'\bmv_\.(read8|read16|read32|read64|read_f32|read_f64|'
                   r'write8|write16|write32|write64|write_f32|write_f64)\(')
RX_PSQ = re.compile(r'\bpsq_(l|st)_fast\(ctx, mv_,')


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def main():
    a = sys.argv[1:]
    i = a.index('--orig'); orig = a[i + 1]; del a[i:i + 2]
    exp, lst, out_tsv = a
    io_pcs = set()
    for ln in open(lst):
        f = ln.rstrip('\n').split('\t')
        if f[0] == 'pc' or not f[0]:
            continue
        io_pcs.add(int(f[0], 16))
    files = sorted((f for f in os.listdir(exp) if re.fullmatch(r'output_\d+\.cpp', f)), key=lambda f: int(f[7:-4]))
    rows, nid, nio, synth, skipped = [], 0, 0, 0, 0
    # site ids must be stable across re-runs: ids are assigned in file order, so a re-run on an
    # already-marked tree re-reads the ids from the existing sites file instead.
    if any(open(os.path.join(exp, f)).readline().startswith(MARK) for f in files):
        if all(open(os.path.join(exp, f)).readline().startswith(MARK) for f in files):
            print('all files already marked: nothing to do'); return
        print('some files marked, some not: restore a fresh copy of the export'); sys.exit(2)
    for fn in files:
        path = os.path.join(exp, fn)
        if sha(path) != sha(os.path.join(orig, fn)):
            print(f'{fn}: sha256 differs from {orig}; refusing'); sys.exit(3)
        text = open(path).read()
        out, func, pc = [], '?', None
        for ln, line in enumerate(text.split('\n'), 1):
            m = RX_FUNC.match(line)
            if m:
                func, pc = m.group(1), None
            m = RX_PC.match(line)
            if m:
                pc = int(m.group(1), 16)

            def site_pc():
                nonlocal synth
                if pc is not None:
                    return pc
                synth += 1
                return 0xAF000000 | synth

            def rep_mv(m):
                nonlocal nid, nio
                p = site_pc()
                if p in io_pcs:
                    nio += 1
                    rows.append(('io', p, m.group(1), func, fn, ln))
                    return f'mv_.{m.group(1)}_io('
                rows.append((nid, p, m.group(1), func, fn, ln))
                s = f'mv_.{m.group(1)}<{nid}u>('
                nid += 1
                return s

            def rep_psq(m):
                nonlocal nid, nio
                p = site_pc()
                k = 'psq_l' if m.group(1) == 'l' else 'psq_st'
                if p in io_pcs:
                    nio += 1
                    rows.append(('io', p, k, func, fn, ln))
                    return f'psq_{m.group(1)}_io(ctx, mv_,'
                rows.append((nid, p, k, func, fn, ln))
                s = f'psq_{m.group(1)}_fast<{nid}u>(ctx, mv_,'
                nid += 1
                return s

            if 'mv_.' in line and not line.lstrip().startswith('nwii::runtime::MemView'):
                line = RX_MV.sub(rep_mv, line)
            if '_fast(ctx, mv_,' in line:
                line = RX_PSQ.sub(rep_psq, line)
            line = line.replace('uint64_t r9_axfx_hash(const std::vector<uint8_t>& m)',
                                'uint64_t r9_axfx_hash(const nwii::runtime::MemBlock& m)')
            out.append(line)
        with open(path, 'w') as f:
            f.write(MARK + ' fastmem site ids / MMIO sites (r9/r12fm/tools/apply_r12_fastmem.py)\n')
            f.write('\n'.join(out))
    with open(out_tsv, 'w') as f:
        f.write('id\tpc\tkind\tfunc\tfile\tline\n')
        for r in rows:
            f.write(f'{r[0]}\t{r[1]:08X}\t{r[2]}\t{r[3]}\t{r[4]}\t{r[5]}\n')
    if nid >= (1 << 24):
        print('too many sites for the 24-bit id field'); sys.exit(4)
    print(f'{len(files)} files: {nid} fastmem sites, {nio} MMIO (_io) sites, {synth} without // 0x -> {out_tsv}')


if __name__ == '__main__':
    main()
