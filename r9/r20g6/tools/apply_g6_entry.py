#!/usr/bin/env python3
"""apply_g6_entry.py: G6 stage 1 pre-pass (before G5's apply_g5_regs.py): no more "every instruction is a
resume label" functions.

  apply_g6_entry.py <export_dir> --jt <jt.txt> --orig <same export, untouched> [--no-jt]

Background. perf/apply_fastdispatch.py put a label `nwii_entry:` in front of the entry switch of every
function that had a `ctx.pc = ctx.ctr; return;` text, and rewrote each non-linking bctr to
    ctx.pc = ctx.ctr; if (ctx.pc >= LO && ctx.pc <= HI) goto nwii_entry; nwii_dispatch(ctx); return;
It counted the bctrl sites too (their text contains the same substring), so 8 347 functions got the label
without any `goto nwii_entry` at all. perf/apply_r8_codegen.py keeps every case of the entry switch of a
function with the label (a case per guest instruction, `default: interpret_step`): every instruction is a
merge point and nothing can stay in a host register (G5 excluded or lost on those functions). On the console
(m1, 4 players) they are 30 % of the generated-code samples (label only 19.6 %, real local bctr 10.2 %).

This pass, per function with the label:
  1. each local bctr becomes
         switch (ctx.ctr) { case 0xT: ctx.pc = 0xT; goto loc_T; ... }
         ctx.pc = ctx.ctr; nwii_dispatch(ctx); return;
     with T = the jump-table targets the analyzer found for this function (NWiiAnalyzer, dumped by
     gen/jtdump into jt.txt; the same tables the recompiler used to emit these labels), or, without a
     table (a tail call through ctr: 2 082 functions),
         ctx.pc = ctx.ctr; nwii_dispatch(ctx); return;
     Exact for every target: a table target jumps to its label as before; anything else goes through
     nwii_dispatch, i.e. the recompiled function holding that address (this one for an in-range address)
     is called with ctx.pc = target and resumes there by its entry switch (or resume_pruned = one
     interpreted instruction, the same as interpret_step did for an address without a case), and then this
     function returns, as after any tail call. If the host stack is low, nwii_dispatch does nothing and
     the return leaves ctx.pc = target for run_game (the usual unwinding).
  2. the label goes and the entry switch is pruned exactly as apply_r8_codegen.py --prune-only does for
     every other function: only the addresses something can resume at keep their case (any guest address
     the generated code stores in ctx.pc/lr/srr0 or compares ctx.pc with, plus every case of run_game's
     table, which includes all jump-table targets); default -> resume_pruned.
--no-jt: step 1 without tables (every bctr through nwii_dispatch); only for experiments.
Marks each output_*.cpp with `// [r20-g6e]` as first line; unmarked files must match --orig (sha256).
"""
import hashlib
import os
import re
import sys

MARK = '// [r20-g6e]'
FUNC = re.compile(r'^void (\w+)\(CPUContext& ctx\) \{$')
BCTR = re.compile(r'^(\s*)ctx\.pc = ctx\.ctr; if \(ctx\.pc >= 0x([0-9A-F]+)u && ctx\.pc <= 0x([0-9A-F]+)u\) goto nwii_entry; '
                  r'nwii_dispatch\(ctx\); return;$')
CASE = re.compile(r'^            case 0x([0-9A-F]+): goto loc_[0-9A-F]+;$')
ENTRY_REF = re.compile(r'(?:ctx\.(?:pc|lr|srr0) = |ctx\.pc [!=]= )0x([0-9A-F]+)')
RUN_GAME_CASE = re.compile(r'^            case 0x([0-9A-F]+): \w+\(ctx\); break;$', re.M)
DEFAULT_OLD = '            default: nwii::runtime::interpret_step(ctx); return;'
DEFAULT_NEW = '            default: nwii::runtime::resume_pruned(ctx); return;'


def collect_entries(d):
    ent = set()
    for x in os.listdir(d):
        if re.match(r'output_\d+\.cpp$', x):
            with open(os.path.join(d, x)) as f:
                for m in ENTRY_REF.finditer(f.read()):
                    ent.add(int(m.group(1), 16))
    with open(os.path.join(d, 'main_output.cpp')) as f:
        src = f.read()
    for m in ENTRY_REF.finditer(src):
        ent.add(int(m.group(1), 16))
    for m in RUN_GAME_CASE.finditer(src):
        ent.add(int(m.group(1), 16))
    if len(ent) < 1000:
        sys.exit('collect_entries: too few entries, refusing to prune')
    return ent


def load_jt(path):
    jt = {}
    for line in open(path):
        p = line.split()
        jt[int(p[0], 16)] = [int(x, 16) for x in p[3:]]
    return jt


def transform(fn, entries, jt, st):
    if '    nwii_entry: ;' in fn:
        raise SystemExit('indented nwii_entry')
    if 'nwii_entry: ;' not in fn:
        return fn
    name = FUNC.match(fn[0]).group(1)
    addr = int(re.search(r'([0-9A-F]{8})$', name).group(1), 16)
    labels = {int(l[4:12], 16) for l in fn if l.startswith('loc_')}
    targets = sorted(t for t in jt.get(addr, []) if t in labels)
    if len(targets) != len(jt.get(addr, [])):
        raise SystemExit(f'{name}: jump-table target without a label')
    out = []
    i = 0
    nb = 0
    while i < len(fn):
        l = fn[i]
        if l == 'nwii_entry: ;':
            i += 1
            continue
        m = BCTR.match(l)
        if m:
            nb += 1
            lo, hi = int(m.group(2), 16), int(m.group(3), 16)
            if any(not (lo <= t <= hi) for t in targets):
                raise SystemExit(f'{name}: target outside the old range')
            if targets:
                # two statements: the table switch reads only ctr (no escape: G5 keeps the registers in
                # locals on the way to the case), the fallback through nwii_dispatch is the escape
                cases = ' '.join('case 0x%X: ctx.pc = 0x%X; goto loc_%08X;' % (t, t, t) for t in targets)
                out.append(f'{m.group(1)}switch (ctx.ctr) {{ {cases} }}')
                out.append(f'{m.group(1)}ctx.pc = ctx.ctr; nwii_dispatch(ctx); return;')
                st['bctr_table'] += 1
            else:
                out.append(f'{m.group(1)}ctx.pc = ctx.ctr; nwii_dispatch(ctx); return;')
                st['bctr_tail'] += 1
            i += 1
            continue
        if 'goto nwii_entry' in l or 'nwii_entry' in l:
            raise SystemExit(f'{name}: unknown nwii_entry use: {l.strip()[:80]}')
        if l.strip().startswith('if (ctx.pc != 0x') and l.endswith('{') and i + 1 < len(fn) and fn[i + 1].strip() == 'switch (ctx.pc) {':
            j = i + 1
            while fn[j] != '    }':
                j += 1
            sw = fn[i:j + 1]
            kept = []
            for b in sw:
                cm = CASE.match(b)
                if cm and int(cm.group(1), 16) not in entries:
                    st['pruned'] += 1
                    continue
                if cm:
                    st['kept'] += 1
                kept.append(DEFAULT_NEW if b == DEFAULT_OLD else b)
            if DEFAULT_NEW not in kept:
                raise SystemExit(f'{name}: entry switch without the interpret_step default')
            out.extend(kept)
            i = j + 1
            continue
        out.append(l)
        i += 1
    st['fns'] += 1
    st['fns_bctr'] += 1 if nb else 0
    return out


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def main():
    a = sys.argv[1:]
    d = a[0]
    jt = load_jt(a[a.index('--jt') + 1])
    if '--no-jt' in a:
        jt = {}
    orig = a[a.index('--orig') + 1]
    entries = collect_entries(d)
    st = dict(fns=0, fns_bctr=0, bctr_table=0, bctr_tail=0, pruned=0, kept=0, files=0)
    for x in sorted(os.listdir(d)):
        if not re.match(r'output_\d+\.cpp$', x):
            continue
        p = os.path.join(d, x)
        with open(p) as f:
            first = f.readline()
        if first.startswith(MARK):
            continue
        if sha(p) != sha(os.path.join(orig, x)):
            sys.exit(f'{x}: sha256 differs from {orig}; refusing')
        src = open(p).read().split('\n')
        res = [MARK + ' no nwii_entry functions: local bctr through the jump table or nwii_dispatch, entry switch pruned (r9/r20g6/tools/apply_g6_entry.py)']
        i = 0
        while i < len(src):
            if FUNC.match(src[i]):
                j = i + 1
                while src[j] != '}':
                    j += 1
                res.extend(transform(src[i:j + 1], entries, jt, st))
                i = j + 1
                continue
            res.append(src[i])
            i += 1
        tmp = p + '.tmp'
        open(tmp, 'w').write('\n'.join(res))
        os.replace(tmp, p)
        st['files'] += 1
    print('g6e:', st, 'entries', len(entries))


if __name__ == '__main__':
    main()
