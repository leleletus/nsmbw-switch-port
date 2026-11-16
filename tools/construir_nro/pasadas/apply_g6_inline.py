#!/usr/bin/env python3
"""apply_g6_inline.py: G6 stage 2 pre-pass (after apply_g6_entry.py, before G5's apply_g5_regs.py):
copy small recompiled callees into their direct call sites.

  apply_g6_inline.py <export_dir> --orig <same export, untouched> [--max-ppc N] [--only callees.txt]
                     [--callers callers.txt] [--stats]
  --only: inline only these callees; --callers: only in these callers (e.g. the functions with samples in
  the console profiles, tools/fnsamples.py); --stats: print what it would do, write nothing.

A direct guest call is
    ctx.lr = 0xRET; // save return address
    ctx.pc = 0xF; F(ctx);
    if (ctx.pc != 0xRET) return;
and costs, besides the native call, the callee's entry check (ctx.pc != F), its MemView, its trace test,
and in G5 code a write-back of every dirty guest register before and a reload of every live one after.
For a small callee F that is several times the work of its body. The call becomes
    ctx.lr = 0xRET; // save return address
    ctx.pc = 0xF; // [g6i] F inlined
    <F's body after its entry switch, labels renamed loc_<K><addr>; each `ctx.pc = ctx.lr; return;` (blr)
     -> `ctx.pc = <lr>; goto loc_<cont>;`>
loc_<cont>: ;
    if (ctx.pc != 0xRET) return;
Exactness (same guest state at every point, same statements in the same order):
  - the callee's body runs with ctx.pc == F, as after the direct call (its entry switch only resumes at
    other addresses, never on this path; its trace test is a debug option and is not copied);
  - a blr leaves ctx.pc = ctx.lr; the continuation is the caller's own check, as after the return. When
    the callee has no escape and never writes lr, lr is still RET there and `ctx.pc = 0xRET` is written
    instead (same value);
  - `if (ctx.pc != 0xX) return;` after the callee's own direct calls goes to the caller's check too;
    the other `return;`s of the callee (the interrupt poll `if (process_pending_callbacks(ctx)) return;`,
    the check inside an indirect call) return from the caller instead of returning to it: with ctx exact there (G5 writes everything back before a return), the caller's caller unwinds and
    run_game resumes at ctx.pc through the entry switches exactly as when the callee returned to the
    caller and its check failed. If ctx.pc happened to be RET, the original continued in the caller and
    this resumes the caller at RET through its entry switch (RET is a kept case: every return address
    stored in ctx.lr is) - the same guest state, only another host path;
  - a tail call of the callee (`...; T(ctx); return;`, `ctx.pc = ctx.ctr; nwii_dispatch(ctx); return;`)
    continues at the caller's check (`...; goto loc_<cont>;`), as the callee's return did; an indirect call
    (bctrl) keeps its own `if (ctx.pc != 0xX) return;` (see above);
  - callees with anything else (top-level declarations, blrl/temp_lr, jump tables) are not inlined.
Marks each output_*.cpp with `// [r20-g6i]` as first line; unmarked files must match --orig (sha256).
"""
import hashlib
import os
import re
import sys

MARK = '// [r20-g6i]'
FUNC = re.compile(r'^void (\w+)\(CPUContext& ctx\) \{$')
CALL = re.compile(r'^    ctx\.pc = 0x([0-9A-F]+); (\w+)\(ctx\);$')
LRSET = re.compile(r'^    ctx\.lr = 0x([0-9A-F]+); // save return address$')
CHECK = re.compile(r'^    if \(ctx\.pc != 0x([0-9A-F]+)\) return;$')
LABEL = re.compile(r'^loc_([0-9A-F]+): ;$')
GOTO = re.compile(r'goto loc_([0-9A-F]+);')
DECL = re.compile(r'^    (const |static )?(u?int\d+_t|double|float|bool|auto|char|unsigned|int|long|nwii::|std::)\b')
BLR = '    ctx.pc = ctx.lr; return;'
CBLR = re.compile(r'^(\s+)if \(([^;{}]*)\) \{ ctx\.pc = ctx\.lr; return; \}$')
POLL = re.compile(r'^\s+if \(process_pending_callbacks\(ctx\)\) return;$')
PCCHECK = re.compile(r'^(\s+)if \(ctx\.pc != 0x([0-9A-F]+)\) return;$')
TAIL = re.compile(r'^(\s+)(ctx\.pc = 0x[0-9A-F]+; if \(nwii::runtime::native_stack_ok\(\)\) \w+\(ctx\);|ctx\.pc = ctx\.ctr; nwii_dispatch\(ctx\);) return;$')
BCTRL = re.compile(r'^\s+if \(ctx\.ctr\) \{ ctx\.pc = ctx\.ctr; nwii_dispatch\(ctx\); if \(ctx\.pc != 0x[0-9A-F]+\) return; \} else nwii::runtime::note_null_call\(0x[0-9A-F]+, ctx\.lr\);$')
ESC = re.compile(r'\bctx\s*[,)]')


def callee_body(fn):
    """Body lines after the entry switch, or None if the function does not have the plain shape."""
    i = 1
    pre = []
    while i < len(fn) - 1 and (fn[i].startswith('    nwii::runtime::MemView mv_(ctx.mmu);') or 'g_trace_calls' in fn[i]):
        pre.append(fn[i]); i += 1
    if not (fn[i].startswith('    if (ctx.pc != 0x') and fn[i + 1].strip() == 'switch (ctx.pc) {'):
        return None
    while fn[i] != '    }':
        i += 1
    return fn[i + 1:-1]


def eligible(name, body, max_ppc):
    if body is None:
        return 'shape'
    ppc = sum(1 for l in body if l.startswith('    // 0x'))
    if ppc > max_ppc:
        return 'size'
    labels = {m.group(1) for l in body for m in [LABEL.match(l)] if m}
    if not body or not LABEL.match(body[0]):
        return 'no entry label'
    # the call enters at the address in the name; the body after the entry switch starts at the
    # function's lowest address. When they differ (e.g. an entry that is a back-branch into the body),
    # the original call went through the entry switch: not inlined.
    m = re.search(r'([0-9A-F]{8})$', name)
    if not m or LABEL.match(body[0]).group(1) != m.group(1):
        return 'entry not first'
    esc = False
    for l in body:
        code = l.split('//')[0] if '//' in l and not l.lstrip().startswith('//') else ('' if l.lstrip().startswith('//') else l)
        s = code.strip()
        if not s:
            continue
        if DECL.match(code) and 'MemView' not in code:
            return 'declaration'
        if 'nwii_entry' in code or 'temp_lr' in code:
            return 'indirect/tail'
        if ('nwii_dispatch' in code or 'native_stack_ok' in code) and not (TAIL.match(l) or BCTRL.match(l)):
            return 'indirect/tail'
        if 'ctx.lr =' in code or 'ctx.lr=' in code:
            esc = True  # writes lr: the continuation uses ctx.lr
        for g in GOTO.findall(code):
            if g not in labels:
                return 'goto outside'
        if 'return' in code:
            if l == BLR or CBLR.match(l) or POLL.match(l) or PCCHECK.match(l) or TAIL.match(l) or BCTRL.match(l):
                pass
            else:
                return 'return form'
        if 'switch' in code or 'case ' in code:
            return 'switch'
        if ESC.search(code.replace('psq_l_fast', 'X').replace('psq_st_fast', 'X')) or '_io(' in code:
            esc = True
    return ('ok', ppc, esc)


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def main():
    a = sys.argv[1:]
    d = a[0]
    orig = a[a.index('--orig') + 1] if '--orig' in a else None
    max_ppc = int(a[a.index('--max-ppc') + 1]) if '--max-ppc' in a else 16
    only = None
    if '--only' in a:
        only = {l.strip() for l in open(a[a.index('--only') + 1]) if l.strip() and not l.startswith('#')}
    callers = None
    if '--callers' in a:
        callers = {l.strip() for l in open(a[a.index('--callers') + 1]) if l.strip() and not l.startswith('#')}
    stats_only = '--stats' in a
    files = sorted((x for x in os.listdir(d) if re.match(r'output_\d+\.cpp$', x)), key=lambda x: int(x[7:-4]))
    srcs = {}
    funcs = {}
    for x in files:
        p = os.path.join(d, x)
        with open(p) as f:
            first = f.readline()
        if first.startswith(MARK):
            sys.exit(f'{x}: already inlined')
        if orig and sha(p) != sha(os.path.join(orig, x)):
            sys.exit(f'{x}: sha256 differs from {orig}; refusing')
        src = open(p).read().split('\n')
        srcs[x] = src
        i = 0
        while i < len(src):
            m = FUNC.match(src[i])
            if m:
                j = i + 1
                while src[j] != '}':
                    j += 1
                funcs[m.group(1)] = src[i:j + 1]
                i = j + 1
                continue
            i += 1
    elig = {}
    why = {}
    for name, fn in funcs.items():
        if only is not None and name not in only:
            continue
        e = eligible(name, callee_body(fn), max_ppc)
        if isinstance(e, tuple):
            elig[name] = (callee_body(fn), e[1], e[2], any('mv_.' in l or 'mv_,' in l for l in fn))
        else:
            why[e] = why.get(e, 0) + 1
    st = dict(sites=0, ppc_inlined=0, callers=0, files=0)
    per_callee = {}
    uid = [0]
    out_files = {}
    for x in files:
        src = srcs[x]
        res = [MARK + ' small callees inlined at their direct call sites (r9/r20g6/tools/apply_g6_inline.py, max %d PPC)' % max_ppc]
        i = 0
        while i < len(src):
            m = FUNC.match(src[i])
            if not m:
                res.append(src[i]); i += 1; continue
            j = i + 1
            while src[j] != '}':
                j += 1
            fn = src[i:j + 1]
            i = j + 1
            caller = m.group(1)
            if callers is not None and caller not in callers:
                res.extend(fn)
                continue
            out = [fn[0]]
            need_mv = False
            has_mv = any(l.startswith('    nwii::runtime::MemView mv_(ctx.mmu);') for l in fn[1:3])
            k = 1
            n_here = 0
            while k < len(fn) - 1:
                l = fn[k]
                cm = CALL.match(l)
                if (cm and cm.group(2) in elig and cm.group(2) != caller and k + 1 < len(fn) and CHECK.match(fn[k + 1])
                        and k >= 1 and LRSET.match(fn[k - 1]) and LRSET.match(fn[k - 1]).group(1) == CHECK.match(fn[k + 1]).group(1)
                        and int(cm.group(1), 16) == int(re.search(r'([0-9A-F]{8})$', cm.group(2)).group(1), 16)):
                    callee = cm.group(2)
                    body, ppc, esc, uses_mv = elig[callee]
                    ret = CHECK.match(fn[k + 1]).group(1)
                    uid[0] += 1
                    tag = '%02X' % (uid[0] % 256) + '%06X' % (uid[0] // 256 % 0x1000000)
                    ren = lambda s: re.sub(r'\bloc_([0-9A-F]{8})\b', lambda mm: 'loc_1%s%s' % (tag, mm.group(1)), s)
                    cont = 'loc_2%s%08X' % (tag, int(ret, 16))
                    lr_expr = 'ctx.lr' if esc else '0x%s' % ret
                    out.append('    ctx.pc = 0x%s; // [g6i] %s inlined (%d PPC)' % (cm.group(1), callee, ppc))
                    for bl in body:
                        if bl == BLR:
                            out.append('    ctx.pc = %s; goto %s;' % (lr_expr, cont)); continue
                        c = CBLR.match(bl)
                        if c:
                            out.append('%sif (%s) { ctx.pc = %s; goto %s; }' % (c.group(1), ren(c.group(2)), lr_expr, cont)); continue
                        c = TAIL.match(bl)
                        if c:  # tail call of the callee: returns to the caller's check
                            out.append('%s%s goto %s;' % (c.group(1), c.group(2), cont)); continue
                        c = PCCHECK.match(bl)
                        if c:
                            out.append('%sif (ctx.pc != 0x%s) goto %s;' % (c.group(1), c.group(2), cont)); continue
                        out.append(ren(bl))
                    out.append(cont + ': ;')
                    out.append(fn[k + 1])
                    need_mv |= uses_mv
                    st['sites'] += 1
                    st['ppc_inlined'] += ppc
                    per_callee[callee] = per_callee.get(callee, 0) + 1
                    n_here += 1
                    k += 2
                    continue
                out.append(l)
                k += 1
            out.append(fn[-1])
            if need_mv and not has_mv:
                out.insert(1, '    nwii::runtime::MemView mv_(ctx.mmu);')
            if n_here:
                st['callers'] += 1
            res.extend(out)
        out_files[x] = res
    print('g6i: eligible callees %d (max %d PPC); not eligible: %s' % (len(elig), max_ppc, why))
    print('g6i:', st)
    top = sorted(per_callee.items(), key=lambda kv: -kv[1])[:30]
    print('g6i: most inlined:', ', '.join('%s x%d' % kv for kv in top))
    if stats_only:
        return
    for x, res in out_files.items():
        p = os.path.join(d, x)
        tmp = p + '.tmp'
        open(tmp, 'w').write('\n'.join(res))
        os.replace(tmp, p)
        st['files'] += 1
    print('g6i: wrote %d files' % st['files'])


if __name__ == '__main__':
    main()
