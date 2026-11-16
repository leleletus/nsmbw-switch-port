#!/usr/bin/env python3
"""apply_g5_regs.py: R14 G5 post-pass (after the r12 fastmem pass): guest registers in C++ locals.

  apply_g5_regs.py <export_dir> --orig <r13 export dir> [--fpr] [--stats out.txt]
  apply_g5_regs.py --file <in.cpp> <out.cpp> [--fpr]        (one file, no marks: experiments)

Every recompiled function `void X(CPUContext& ctx) {` keeps the guest state below in locals
(G5r3, G5c0eq, G5xer, G5ctr, G5ic, with --fpr also G5f1 / G5p1) instead of ctx, so GCC can keep
them in host registers (with fastmem a guest access is one asm instruction, no call):

  ctx.gpr[N]  ctx.cr[N].{lt,gt,eq,so}  ctx.xer  ctx.ctr  ctx.inst_count  [ctx.fpr[N] ctx.ps1[N]]

Not cached: pc, lr, msr, srr0/1, gqr, fpscr, ... (rare or written right before an escape).

Exactness (as R8's apply_reglocals.py, which this extends; difftest-verified there):
  - locals are loaded from ctx at the function entry (before the entry switch: ctx is exact at
    any entry); GCC drops the loads that are dead;
  - "spill" (ctx.X = local) of the registers that may be dirty (forward may-analysis over the
    function's goto CFG) before anything that can look at ctx: guest calls, nwii_dispatch,
    interpret_step, resume_pruned, micro_interpret, idle_wait, HLE stubs, unknown ctx members,
    blocks that index registers with a variable (lmw/stmw/mfcr/mtcrf loops), returns;
  - "reload" (local = ctx.X) after them of the registers live afterwards (backward liveness;
    a spill counts as a use, so a later spill never writes back a stale local);
  - the interrupt poll at back-edges (`if (process_pending_callbacks(ctx)) return;`) is split:
    the inline countdown/check (nwii::runtime::poll_due, no ctx register is read) runs without
    any spill; only when it says "slow" are the dirty registers written back, then
    process_pending_callbacks_slow (which may deliver an interrupt / longjmp) and the live
    registers reloaded;
  - partial effects: MMIO calls (mv_.X_io, psq_*_io) and mftb/mfdec/mtdec read inst_count
    (virtual clock): the local is written back first if dirty. fcmp/ps_cmp are expanded inline
    (they write a CR field). psq_l_fast/psq_st_fast only touch fpr/ps1/gqr (with --fpr: spill /
    reload of that FPR).
Functions with a statement shape this pass does not know are left unchanged (counted).

Safety (as the r9/r12 passes): each output_*.cpp gets `// [r14-g5]` as first line; marked files
are skipped (idempotent); unmarked files must have the sha256 of the same file in --orig.
"""
import hashlib, os, re, sys

MARK = '// [r14-g5]'
FPR = False
UNROLL = True  # --no-unroll: keep lmw/stmw/mfcr/mtcrf loops as write-back/reload blocks (round 1)

REF_BASE = r'ctx\.gpr\[(\d+)\]|ctx\.cr\[(\d)\]\.(lt|gt|eq|so)|ctx\.xer_ca\b|ctx\.xer\b|ctx\.ctr\b|ctx\.inst_count\b'
# XER is kept as two locals: G5ca (the CA bit, written by every carrying op: most of it is dead)
# and G5xer (SO/OV/byte count; bit 29 of it is never used). The pass first rewrites the CA
# updates/reads of the generated code to a pseudo field ctx.xer_ca (below); mfxer/mtxer use the
# combined value.
XER_REWRITES = [
    (re.compile(r'ctx\.xer = \(ctx\.xer & ~\(1u? << 29\)\) \| \(\(\(res >> 32\) & 1\) << 29\);'), 'ctx.xer_ca = ((res >> 32) & 1);'),
    (re.compile(r'ctx\.xer = \(ctx\.xer & ~\(1u << 29\)\) \| \(ca << 29\);'), 'ctx.xer_ca = (ca);'),
    (re.compile(r'ctx\.xer = \(ctx\.xer & ~\(1u << 29\)\) \| \(\(uint32_t\)\(res >> 32\) & 1u\) << 29;'), 'ctx.xer_ca = ((uint32_t)(res >> 32) & 1u);'),
    (re.compile(r'\(ctx\.xer >> 29\) & 1\b'), 'ctx.xer_ca'),
    (re.compile(r'^(    ctx\.gpr\[\d+\]) = ctx\.xer;'), r'\1 = (ctx.xer & ~(1u << 29)) | ((uint32_t)ctx.xer_ca << 29);'),
    (re.compile(r'^    ctx\.xer = (ctx\.gpr\[\d+\]);'), r'    ctx.xer = \1; ctx.xer_ca = (\1 >> 29) & 1;'),
]
XER_LEFT = re.compile(r'ctx\.xer\b(?! >> 31\) & 1;)(?!_ca)')
REF_FPR = r'|ctx\.(fpr|ps1)\[(\d+)\]'
REF = re.compile(REF_BASE)
ASSIGN_AFTER = re.compile(r'\s*(=(?!=)|\+=|-=|\*=|/=|\|=|&=|\^=|<<=|>>=|\+\+|--)')
ESCAPE = re.compile(r'\bctx\s*[,)]')
# calls that take ctx but only touch fpr/ps1/gqr/mmu (masked before the escape test)
SAFE_CTX_CALL = re.compile(r'(\bpsq_(?:l|st)_(?:fast<\d+u>|io)\()ctx,')
PSQ_CALL = re.compile(r'\bpsq_(l|st)_(fast<\d+u>|io)\(ctx, mv_, (\d+), ')
LABEL = re.compile(r'^(loc_[0-9A-F]+|nwii_entry): ;$')
GOTO = re.compile(r'goto (loc_[0-9A-F]+|nwii_entry);')
CTX_MEMBER = re.compile(r'\bctx\.([a-z_0-9]+)')
SAFE_FIELDS = {'gpr', 'cr', 'xer', 'xer_ca', 'ctr', 'pc', 'lr', 'inst_count', 'mmu', 'fpr', 'ps1', 'gqr', 'msr',
               'srr0', 'srr1', 'hid2', 'dma_u', 'dma_l', 'sprg', 'fpscr', 'dispatch_saved_ctx',
               'reservation_addr'}
# CPUContext methods that touch none of the cached state except as noted (checked against
# cpu_context.h: only fcmp/ps_cmp* write cr; read_timebase/read_dec/write_dec read inst_count).
SAFE_HELPERS = {'lfs', 'stfs', 'fmuls', 'fsubs', 'fadds', 'psq_load', 'psq_store', 'fdivs', 'fctiwz',
                'fctiw', 'frsp', 'ps_muls0', 'ps_sub', 'ps_mul', 'ps_add', 'ps_madd', 'ps_sum0',
                'ps_madds0', 'ps_mr', 'fmadds', 'ps_merge00', 'ps_nmsub', 'ps_merge10', 'fres',
                'ps_msub', 'ps_neg', 'ps_madds1', 'ps_merge11', 'ps_merge01', 'ps_muls1', 'ps_abs',
                'fnmsubs', 'fnmadds', 'ps_sum1', 'frsqrte', 'fmsubs', 'mffs', 'write_hid2', 'write_dmal',
                'ps_nmadd', 'mtfsf', 'mtfsfi', 'dcbz', 'write_dec', 'read_dec', 'mtfsb', 'read_timebase',
                'ps_div', 'ps_res', 'ps_rsqrte', 'ps_sel', 'ps_nabs', 'fmadd', 'fmsub', 'fnmadd',
                'fnmsub', 'stfiwx', 'set_fpscr', 'fill', 'set_bits'}
# with --fpr every other helper that is still a call is an escape, except these (no FPR):
NOFP_HELPERS = {'write_hid2', 'write_dmal', 'dcbz', 'write_dec', 'read_dec', 'read_timebase'}
IC_READERS = re.compile(r'ctx\.(read_timebase|read_dec|write_dec)\(|\b_io\(|\w+_io\(')
FCMP = re.compile(r'ctx\.fcmp\((\d), (ctx\.(?:fpr|ps1)\[\d+\]), (ctx\.(?:fpr|ps1)\[\d+\])\);')
PSCMP = re.compile(r'ctx\.ps_cmp[uo]([01])\((\d), (\d+), (\d+)\);')
POLL = '    if (process_pending_callbacks(ctx)) return;'
UNCOND = [re.compile(p) for p in (
    r'^goto \w+;$',
    r'^ctx\.pc = ctx\.lr; return;$',
    r'^return;$',
    r'^ctx\.pc = ctx\.ctr; if \(ctx\.pc >= 0x[0-9A-F]+u && ctx\.pc <= 0x[0-9A-F]+u\) goto nwii_entry; nwii_dispatch\(ctx\); return;$',
    r'^ctx\.pc = 0x[0-9A-F]+; if \(nwii::runtime::native_stack_ok\(\)\) [\w:]+\(ctx\); return;$',
)]
COND_OK = [re.compile(p) for p in (
    r'^if \([^;{}]*\) goto loc_[0-9A-F]+;$',
    r'^if \(ctx\.pc != 0x[0-9A-F]+\) return;$',
    r'^if \([^;{}]*\) \{ ctx\.pc = ctx\.lr; return; \}$',
    r'^if \(process_pending_callbacks\(ctx\)\) return;$',
    r'^if \(ctx\.ctr\) \{ ctx\.pc = ctx\.ctr; nwii_dispatch\(ctx\); if \(ctx\.pc != 0x[0-9A-F]+\) return; \} else nwii::runtime::note_null_call\(0x[0-9A-F]+, ctx\.lr\);$',
    r'^if \([^;{}]*\) \{ nwii::runtime::idle_wait\(ctx\); goto loc_[0-9A-F]+; \}$',
    r'^ctx\.pc = temp_lr; nwii_dispatch\(ctx\); if \(ctx\.pc != 0x[0-9A-F]+\) return;$',
)]
COND_START = ('if ', 'if(', 'else', '} else', 'for ', 'for(', 'while', 'switch', 'do ')


class Skip(Exception):
    pass


def lname(key):
    k = key[0]
    if k == 'g': return 'G5r%d' % key[1]
    if k == 'f': return 'G5f%d' % key[1]
    if k == 'p': return 'G5p%d' % key[1]
    if k == 'c': return 'G5c%d%s' % (key[1], key[2])
    if k == 'x': return 'G5xer'
    if k == 'a': return 'G5ca'
    if k == 'i': return 'G5ic'
    return 'G5ctr'


def cname(key):
    k = key[0]
    if k == 'g': return 'ctx.gpr[%d]' % key[1]
    if k == 'f': return 'ctx.fpr[%d]' % key[1]
    if k == 'p': return 'ctx.ps1[%d]' % key[1]
    if k == 'c': return 'ctx.cr[%d].%s' % (key[1], key[2])
    if k == 'x': return 'ctx.xer'
    if k == 'a': return 'ctx.xer_ca'
    if k == 'i': return 'ctx.inst_count'
    return 'ctx.ctr'


def load_expr(key):
    if key[0] == 'a': return '(ctx.xer >> 29) & 1'
    return cname(key)


def store_stmt(key):
    if key[0] == 'a': return 'ctx.xer = (ctx.xer & ~(1u << 29)) | ((uint32_t)G5ca << 29);'
    if key[0] == 'x': return 'ctx.xer = (G5xer & ~(1u << 29)) | (ctx.xer & (1u << 29));'
    return '%s = %s;' % (cname(key), lname(key))


def ctype(key):
    k = key[0]
    if k in 'fp': return 'double'
    if k == 'c': return 'bool'
    if k == 'i': return 'uint64_t'
    return 'uint32_t'


def mkey(m):
    if m.group(1) is not None: return ('g', int(m.group(1)))
    if m.group(2) is not None: return ('c', int(m.group(2)), m.group(3))
    if m.lastindex and m.lastindex >= 4 and m.group(4) is not None:
        return ('f' if m.group(4) == 'fpr' else 'p', int(m.group(5)))
    t = m.group(0)
    if t == 'ctx.xer': return ('x',)
    if t == 'ctx.xer_ca': return ('a',)
    if t == 'ctx.ctr': return ('t',)
    if t == 'ctx.inst_count': return ('i',)
    raise Skip('mkey ' + t)


def strip_comment(s):
    i = s.find('//')
    return s[:i].rstrip() if i >= 0 else s.rstrip()


def rewrite_fcmp(line):
    def p(m):
        half = 'fpr' if m.group(1) == '0' else 'ps1'
        return 'ctx.fcmp(%s, ctx.%s[%s], ctx.%s[%s]);' % (m.group(2), half, m.group(3), half, m.group(4))
    if 'ps_cmp' in line:
        line = PSCMP.sub(p, line)
    def f(m):
        n, a, b = m.group(1), m.group(2), m.group(3)
        return ('{ const double fa_ = %s, fb_ = %s; const bool un_ = std::isnan(fa_) || std::isnan(fb_); '
                'ctx.cr[%s].lt = !un_ && fa_ < fb_; ctx.cr[%s].gt = !un_ && fa_ > fb_; '
                'ctx.cr[%s].eq = !un_ && fa_ == fb_; ctx.cr[%s].so = un_; }' % (a, b, n, n, n, n))
    return FCMP.sub(f, line)


# ---- --fpr: inline expansion of the CPUContext FP helpers ---------------------------------
# The bodies are taken from cpu_context.h (same statements, same order), with the register
# number parameters replaced by the constants of the call, the double/uint64 parameters bound
# to fresh consts first (as a call evaluates its arguments first), and fpr[]/ps1[]/cr[]/fpscr
# made ctx.* again, so the renaming below turns them into locals.
EXPAND = {'fill', 'set_ps', 'fadds', 'fsubs', 'fmuls', 'fdivs', 'fres', 'fmadds', 'fmsubs', 'fnmadds',
          'fnmsubs', 'frsp', 'fmadd', 'fmsub', 'fnmadd', 'fnmsub', 'frsqrte', 'fctiw', 'fctiwz', 'fcmp',
          'ps_add', 'ps_sub', 'ps_mul', 'ps_div', 'ps_madd', 'ps_msub', 'ps_nmadd', 'ps_nmsub', 'ps_sum0',
          'ps_sum1', 'ps_muls0', 'ps_muls1', 'ps_madds0', 'ps_madds1', 'ps_res', 'ps_rsqrte', 'ps_sel',
          'ps_mr', 'ps_neg', 'ps_abs', 'ps_nabs', 'ps_merge00', 'ps_merge01', 'ps_merge10', 'ps_merge11',
          'ps_cmpu0', 'ps_cmpo0', 'ps_cmpu1', 'ps_cmpo1', 'set_bits'}
METHODS = {}
_uid = [0]


def split_args(s, i):
    """s[i] is just after '('. Returns (args, index after ')')."""
    depth, cur, args = 0, '', []
    while i < len(s):
        ch = s[i]
        if ch in '([{': depth += 1
        elif ch in ')]}':
            if depth == 0:
                if cur.strip() or args: args.append(cur.strip())
                return args, i + 1
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(cur.strip()); cur = ''
        else:
            cur += ch
        i += 1
    raise Skip('unbalanced call')


def load_methods(header):
    src = open(header).read()
    a = src.index('struct CPUContext {')
    for m in re.finditer(r'inline void (\w+)\(([^)]*)\) \{', src[a:]):
        name = m.group(1)
        if name not in EXPAND or name in METHODS:
            continue
        j = a + m.end()
        depth, k = 1, j
        while depth:
            if src[k] == '{': depth += 1
            elif src[k] == '}': depth -= 1
            k += 1
        body = ' '.join(src[j:k - 1].split())
        params = []
        for p_ in m.group(2).split(','):
            t, n_ = p_.strip().rsplit(' ', 1)
            params.append((t, n_))
        if 'memcpy' in body or 'return' in body or '&' in body.replace('&&', ''):
            if name != 'set_bits':
                raise SystemExit('helper %s: unexpected body %s' % (name, body))
        METHODS[name] = (params, body)
    missing = EXPAND - set(METHODS)
    if missing:
        raise SystemExit('helpers not found in %s: %s' % (header, sorted(missing)))
    # set_bits(D, v) is std::memcpy(&fpr[D], &v, 8): the same as a bit_cast store.
    METHODS['set_bits'] = ([('uint32_t', 'D'), ('uint64_t', 'v')], 'fpr[D] = std::bit_cast<double>(v);')


INNER = re.compile(r'(?<![\w.:])(' + '|'.join(sorted(EXPAND, key=len, reverse=True)) + r')\(')


def expand(name, args, depth=0):
    if depth > 6:
        raise Skip('helper recursion')
    params, body = METHODS[name]
    if len(args) != len(params):
        raise Skip('helper arity ' + name)
    pre = []
    for (t, pn), av in zip(params, args):
        if t == 'uint32_t':
            if not re.fullmatch(r'\d+', av):
                raise Skip('helper reg arg ' + name)
            body = re.sub(r'\b%s\b' % pn, av, body)
        else:
            _uid[0] += 1
            nn = '_g5%s%d' % (pn, _uid[0])
            pre.append('const %s %s = %s;' % (t, nn, av))
            body = re.sub(r'\b%s\b' % pn, nn, body)
    body = re.sub(r'(?<![\w.])(fpr|ps1|cr)\[', r'ctx.\1[', body)
    body = re.sub(r'(?<![\w.])fpscr\b', 'ctx.fpscr', body)
    # inner helper statements (fill / set_ps / set_bits / fcmp)
    out, i = '', 0
    while True:
        m = INNER.search(body, i)
        if not m:
            out += body[i:]; break
        a2, j = split_args(body, m.end())
        if body[j:j + 1] != ';':
            raise Skip('inner helper not a statement')
        out += body[i:m.start()] + expand(m.group(1), a2, depth + 1)
        i = j + 1
    return '{ ' + ' '.join(pre + [out.strip()]) + ' }'


CTX_CALL = re.compile(r'ctx\.(\w+)\(')


def expand_fp(line):
    """Expands the statement-level ctx.<helper>(...); calls of a generated line."""
    code_end = line.find('//')
    out, i = '', 0
    while True:
        m = CTX_CALL.search(line, i)
        if not m or (code_end >= 0 and m.start() > code_end):
            out += line[i:]; break
        if m.group(1) not in EXPAND:
            out += line[i:m.end()]; i = m.end(); continue
        args, j = split_args(line, m.end())
        if line[j:j + 1] != ';':
            raise Skip('helper not a statement')
        out += line[i:m.start()] + expand(m.group(1), args)
        i = j + 1
    return out


FOR_I = re.compile(r'^(\s*)for \(int i = (\d+); i (<=|<) (\d+); \+\+i\) \{$')


def unroll_loops(body):
    """lmw/stmw/mfcr/mtcrf loops `for (int i = A; i < B; ++i) { ... }` -> one `{ ... }` per i
    with i replaced by the constant (same statements, same order), so the registers they touch
    have constant indices and stay in locals (no write-back/reload of everything around them)."""
    out, i = [], 0
    while i < len(body):
        m = FOR_I.match(body[i])
        if not m:
            out.append(body[i]); i += 1; continue
        ind = m.group(1)
        j = i + 1
        while j < len(body) and body[j] != ind + '}':
            j += 1
        if j >= len(body):
            raise Skip('unroll: no end')
        inner = body[i + 1:j]
        if any(FOR_I.match(x) or 'goto' in x or 'return' in x or 'break' in x or 'continue' in x for x in inner):
            raise Skip('unroll: inner flow')
        a, b = int(m.group(2)), int(m.group(4)) + (1 if m.group(3) == '<=' else 0)
        for v in range(a, b):
            out.append(ind + '{')
            out.extend(re.sub(r'\bi\b', str(v), x) for x in inner)
            out.append(ind + '}')
        i = j + 1
    return out


def cond_flags(body):
    """Per line: True when the statement may not run (inside an if/else/loop body or a
    single-line if). Bare `{` blocks (one guest instruction) are unconditional."""
    out, stack = [], []
    for l in body:
        s = strip_comment(l).strip()
        starts = s.startswith(COND_START)
        out.append(any(stack) or starts)
        opens, closes = s.count('{'), s.count('}')
        if s.startswith('}') and closes:
            if stack: stack.pop()
            closes -= 1
            if s.endswith('{') and opens:
                stack.append(True); opens -= 1  # } else {
        # balanced braces on the same line: no change
        net = opens - closes
        for _ in range(max(net, 0)):
            stack.append(starts)
        for _ in range(max(-net, 0)):
            if stack: stack.pop()
    return out


def transform_function(lines, stats):
    body = lines[1:-1]
    if not any(REF.search(l) for l in body):
        stats['noref'] += 1
        return lines
    if UNROLL:
        body = unroll_loops(body)
    nb = []
    for l in body:
        if 'ctx.xer' in l:
            for rx, rp in XER_REWRITES[:4]:
                l = rx.sub(rp, l)
            c_ = strip_comment(l)
            if XER_LEFT.search(c_) and not re.fullmatch(r'    (ctx\.gpr\[\d+\] = ctx\.xer|ctx\.xer = ctx\.gpr\[\d+\]);', c_):
                raise Skip('xer form')
            for rx, rp in XER_REWRITES[4:]:
                l = rx.sub(rp, l)
        nb.append(l)
    body = nb
    if FPR:
        body = [expand_fp(l) if 'ctx.' in l and '(' in l else l for l in body]
    else:
        body = [rewrite_fcmp(l) if ('fcmp' in l or 'ps_cmp' in l) else l for l in body]
    if any('fcmp(' in strip_comment(l) or 'ps_cmp' in strip_comment(l) for l in body):
        raise Skip('fcmp form')
    head, i = [], 0
    while i < len(body) and (body[i].startswith('    nwii::runtime::MemView mv_(ctx.mmu);') or
                             'g_trace_calls' in body[i]):
        head.append(body[i]); i += 1
    body = body[i:]
    cflag = cond_flags(body)
    nodes = []
    n = len(body)
    i = 0
    while i < n:
        l = body[i]
        s = l.strip()
        if s.startswith('if (ctx.pc != 0x') and s.endswith('{') and i + 1 < n and body[i + 1].strip() == 'switch (ctx.pc) {':
            j = i + 1
            while body[j] != '    }':
                j += 1
            for b in body[i:j + 1]:
                for m in REF.finditer(b):
                    raise Skip('ref in switch')
            nodes.append({'kind': 'switch', 'lines': body[i:j + 1]})
            i = j + 1
            continue
        if l == '    {':
            j = i + 1
            while not body[j].startswith('    }'):
                j += 1
            blk = body[i:j + 1]
            var = r'ctx\.(gpr|cr|fpr|ps1)\[[a-z_(]' if FPR else r'ctx\.(gpr|cr)\[[a-z_(]'
            if any(re.search(var, b) or 'for (' in b or 'while' in b for b in blk):
                for b in blk:
                    c = strip_comment(b)
                    if 'goto' in c or 'return' in c or ESCAPE.search(SAFE_CTX_CALL.sub(r'\1CTX,', c)):
                        raise Skip('raw block with control flow / escape')
                nodes.append({'kind': 'raw', 'lines': blk})
                i = j + 1
                continue
        nodes.append({'kind': 'line', 'lines': [l], 'cond': cflag[i]})
        i += 1
    keys = {}
    label_idx = {}
    for k, nd in enumerate(nodes):
        if nd['kind'] != 'line':
            for b in nd['lines']:
                for m in REF.finditer(b):
                    keys.setdefault(mkey(m), None)
            continue
        l = nd['lines'][0]
        code = strip_comment(l)
        s = code.strip()
        lm = LABEL.match(s)
        if lm:
            if l != s:
                raise Skip('indented label')
            nd['kind'] = 'label'; nd['name'] = lm.group(1); label_idx[lm.group(1)] = k
            continue
        for m in REF.finditer(code):
            keys.setdefault(mkey(m), None)
        if l == POLL:
            nd['kind'] = 'poll'
            nd['uncond'] = False; nd['gotos'] = []
            continue
        masked = SAFE_CTX_CALL.sub(r'\1CTX,', code)
        esc = bool(ESCAPE.search(masked))
        for m in CTX_MEMBER.finditer(code):
            name = m.group(1)
            if name in SAFE_FIELDS: continue
            if name in SAFE_HELPERS and code[m.end():m.end() + 1] == '(' and not (FPR and name not in NOFP_HELPERS):
                continue
            esc = True
        has_flow = ('return' in code) or ('goto' in code)
        uncond = any(p.match(s) for p in UNCOND)
        if has_flow and not uncond and not any(p.match(s) for p in COND_OK):
            raise Skip('unknown flow: ' + s[:50])
        if uncond and nd['cond']:
            raise Skip('nested unconditional flow')
        nd['uncond'] = uncond
        nd['gotos'] = GOTO.findall(code)
        nd['ic'] = bool(IC_READERS.search(code))
        nd['psq'] = PSQ_CALL.findall(code) if FPR else []
        if esc:
            nd['kind'] = 'esc'
            gm = re.search(r'goto (loc_[0-9A-F]+|nwii_entry);', code)
            em = ESCAPE.search(masked)
            if gm and not em:
                raise Skip('escape by member with goto')
            nd['reload_before_goto'] = bool(gm and em and em.start() < gm.start())
            nd['goto_bypasses'] = bool(gm and em and gm.start() < em.start())
        else:
            nd['kind'] = 'pure'
            nd['ret'] = 'return' in code
    keylist = sorted(keys, key=lambda k: (k[0], k[1:] if len(k) > 1 else ()))
    bit = {k: 1 << b for b, k in enumerate(keylist)}
    icbit = bit.get(('i',), 0)
    for nd in nodes:
        if nd['kind'] != 'pure': continue
        code = strip_comment(nd['lines'][0])
        d = u = 0
        for m in REF.finditer(code):
            b = bit[mkey(m)]
            after = ASSIGN_AFTER.match(code, m.end())
            pre = code[max(0, m.start() - 2):m.start()]
            if after and after.group(1) == '=':
                d |= b
                if nd['cond']:
                    u |= b
            elif after or pre in ('++', '--'):
                d |= b; u |= b
            else:
                u |= b
        nd['def'] = d; nd['use'] = u
        hrw = icbit if nd['ic'] else 0
        hw = 0
        for kind, form, reg in nd['psq']:
            r = int(reg)
            fb = bit.get(('f', r), 0) | bit.get(('p', r), 0)
            if kind == 'l': hw |= fb       # psq_l writes fpr/ps1[D]
            else: hrw |= fb                 # psq_st reads them
        nd['hrw'] = hrw; nd['hw'] = hw
        if (hrw or hw) and (nd['ret'] or nd['gotos']):
            raise Skip('partial effect on a flow line')
    N = len(nodes)
    labels = [label_idx[x] for x in label_idx if x != 'nwii_entry']
    succ = [[] for _ in range(N)]
    for k, nd in enumerate(nodes):
        kd = nd['kind']
        if kd == 'line':
            raise Skip('internal')
        if kd in ('pure', 'esc', 'poll'):
            for g in nd['gotos']:
                if g == 'nwii_entry':
                    succ[k].extend(labels)
                else:
                    if g not in label_idx:
                        raise Skip('goto outside function ' + g)
                    succ[k].append(label_idx[g])
            if not nd['uncond'] and k + 1 < N:
                succ[k].append(k + 1)
        elif k + 1 < N:
            succ[k].append(k + 1)
    pred = [[] for _ in range(N)]
    for k in range(N):
        for t in succ[k]:
            pred[t].append(k)
    # forward may-dirty
    din = [0] * N; dout = [0] * N
    work = list(range(N)); inq = [True] * N
    while work:
        k = work.pop(); inq[k] = False
        x = 0
        for p in pred[k]:
            x |= dout[p]
        din[k] = x
        nd = nodes[k]; kd = nd['kind']
        if kd == 'pure':
            o = (x if nd['cond'] else (x & ~nd['hrw'])) | nd['def']
        elif kd == 'esc':
            o = x if nd['cond'] else 0
        elif kd == 'raw':
            o = 0
        else:  # poll (fast path keeps the dirt), label, switch
            o = x
        if o != dout[k]:
            dout[k] = o
            for t in succ[k]:
                if not inq[t]:
                    inq[t] = True; work.append(t)
    # backward liveness (spills read the dirty set)
    lin = [0] * N; lout = [0] * N
    work = list(range(N)); inq = [True] * N
    while work:
        k = work.pop(); inq[k] = False
        x = 0
        for t in succ[k]:
            x |= lin[t]
        lout[k] = x
        nd = nodes[k]; kd = nd['kind']
        if kd == 'pure':
            kill = nd['def'] | (0 if nd['cond'] else nd['hw'])
            i_ = nd['use'] | (nd['hrw'] & din[k]) | (x & ~kill)
            if nd['ret']:
                i_ |= din[k]
        elif kd == 'esc':
            i_ = din[k]
            if nd['goto_bypasses'] or nd['cond']:
                i_ |= x
        elif kd == 'raw':
            i_ = din[k]
        elif kd == 'poll':
            i_ = din[k] | x
        else:
            i_ = x
        if i_ != lin[k]:
            lin[k] = i_
            for p in pred[k]:
                if not inq[p]:
                    inq[p] = True; work.append(p)

    def spill(mask):
        return ''.join(store_stmt(kk) + ' ' for kk in keylist if mask & bit[kk])

    def reload(mask):
        return ''.join('%s = %s; ' % (lname(kk), load_expr(kk)) for kk in keylist if mask & bit[kk])

    def rename(code):
        return REF.sub(lambda m: lname(mkey(m)), code)

    out = [lines[0]] + head
    out.append('    ' + ' '.join('%s %s = %s;' % (ctype(kk), lname(kk), load_expr(kk)) for kk in keylist) + '  // [r14-g5]')
    for k, nd in enumerate(nodes):
        kd = nd['kind']
        if kd in ('switch', 'label'):
            out.extend(nd['lines']); continue
        if kd in ('raw', 'esc') and any('xer_ca' in b for b in nd['lines']):
            raise Skip('xer_ca in raw/esc ' + nd['lines'][0][:60])
        if kd == 'raw':
            sp = spill(din[k])
            if sp: out.append('    ' + sp.rstrip())
            out.extend(nd['lines'])
            rl = reload(lout[k])
            if rl: out.append('    ' + rl.rstrip())
            stats['raw'] += 1
            continue
        if kd == 'poll':
            stats['poll'] += 1
            out.append('    if (NWII_UNLIKELY(nwii::runtime::poll_due(ctx))) { %sif (nwii::runtime::process_pending_callbacks_slow(ctx)) return; %s}'
                       % (spill(din[k]), reload(lout[k])))
            continue
        l = nd['lines'][0]
        ind = l[:len(l) - len(l.lstrip())]
        body_ = l.lstrip()
        if kd == 'esc':
            stats['esc'] += 1
            sp = spill(din[k])
            rl = reload(lout[k])
            if nd['reload_before_goto'] and rl:
                # `if (<escape>) goto X;` has no braces: the reload must stay inside the if
                # (a bare `if (c) a; b; goto X;` would make the goto unconditional).
                gm2 = re.search(r'goto (loc_[0-9A-F]+);', body_)
                if body_[:gm2.start()].rstrip().endswith(')'):
                    body_ = body_[:gm2.start()] + '{ ' + rl + gm2.group(0) + ' }' + body_[gm2.end():]
                else:
                    body_ = body_[:gm2.start()] + rl + body_[gm2.start():]
            ci = body_.find('//')
            c_, cm_ = (body_[:ci].rstrip(), ' ' + body_[ci:]) if ci >= 0 else (body_, '')
            line = ind + sp + c_
            if not nd['uncond'] and rl:
                line += ' ' + rl.rstrip()
            out.append(line + cm_)
            continue
        code = rename(body_)
        if nd['ret'] and din[k]:
            sp = spill(din[k])
            if 'ctx.pc = ctx.lr; return;' in code:
                code = code.replace('ctx.pc = ctx.lr; return;', sp + 'ctx.pc = ctx.lr; return;', 1)
            elif re.match(r'^if \(ctx\.pc != 0x[0-9A-F]+\) return;', code):
                code = code.replace(') return;', ') { ' + sp + 'return; }', 1)
            elif code.startswith('return;'):
                code = sp + code
            else:
                raise Skip('pure return form: ' + code[:50])
            stats['retspill'] += 1
        if nd['hrw'] or nd['hw']:
            ci = code.find('//')
            c_, cm_ = (code[:ci].rstrip(), ' ' + code[ci:]) if ci >= 0 else (code, '')
            sp = spill(nd['hrw'] & din[k])
            rl = reload(nd['hw'] & lout[k])
            if sp or rl:
                stats['partial'] += 1
            code = sp + c_ + ((' ' + rl.rstrip()) if rl else '') + cm_
        out.append(ind + code)
    out.append(lines[-1])
    stats['done'] += 1
    return out


def process_text(src, stats):
    res = []
    i, n = 0, len(src)
    while i < n:
        l = src[i]
        if l.startswith('void ') and l.endswith('(CPUContext& ctx) {'):
            j = i + 1
            while src[j] != '}':
                j += 1
            fn = src[i:j + 1]
            try:
                res.extend(transform_function(fn, stats))
            except Skip as e:
                stats['skip'] += 1
                w = stats.setdefault('why', {})
                w[str(e)[:60]] = w.get(str(e)[:60], 0) + 1
                res.extend(fn)
            i = j + 1
            continue
        res.append(l)
        i += 1
    return res


def sha(p):
    return hashlib.sha256(open(p, 'rb').read()).hexdigest()


def one_file(path):
    st = {'done': 0, 'skip': 0, 'noref': 0, 'esc': 0, 'raw': 0, 'retspill': 0, 'poll': 0, 'partial': 0}
    src = open(path).read().split('\n')
    res = [MARK + (' guest registers in locals%s%s (r9/r14g5/tools/apply_g5_regs.py)' % (' + FPR' if FPR else '', '' if UNROLL else ', no unroll'))]
    res += process_text(src, st)
    tmp = path + '.tmp'
    with open(tmp, 'w') as f:
        f.write('\n'.join(res))
    os.replace(tmp, path)
    return st


def main():
    global FPR, REF
    a = sys.argv[1:]
    global UNROLL
    if '--no-unroll' in a:
        UNROLL = False; a.remove('--no-unroll')
    if '--fpr' in a:
        FPR = True; a.remove('--fpr')
        REF = re.compile(REF_BASE + REF_FPR)
        hdr = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'nWiiRuntime', 'include', 'runtime', 'cpu_context.h')
        if '--header' in a:
            k = a.index('--header'); hdr = a[k + 1]; del a[k:k + 2]
        load_methods(hdr)
    stats = {'done': 0, 'skip': 0, 'noref': 0, 'esc': 0, 'raw': 0, 'retspill': 0, 'poll': 0, 'partial': 0}
    if a[0] == '--file':
        src = open(a[1]).read().split('\n')
        open(a[2], 'w').write('\n'.join(process_text(src, stats)))
    else:
        i = a.index('--orig'); orig = a[i + 1]; del a[i:i + 2]
        exp = a[0]
        files = sorted((f for f in os.listdir(exp) if re.fullmatch(r'output_\d+\.cpp', f)), key=lambda f: int(f[7:-4]))
        todo = []
        for fn in files:
            path = os.path.join(exp, fn)
            with open(path) as f:
                first = f.readline()
            if first.startswith(MARK):
                stats['already'] = stats.get('already', 0) + 1
                continue
            if sha(path) != sha(os.path.join(orig, fn)):
                print(f'{fn}: sha256 differs from {orig}; refusing'); sys.exit(3)
            todo.append(path)
        import multiprocessing
        with multiprocessing.Pool(int(os.environ.get('G5_JOBS', '4'))) as pool:
            for st in pool.map(one_file, todo):
                for k, v in st.items():
                    if k == 'why':
                        w = stats.setdefault('why', {})
                        for k2, v2 in v.items(): w[k2] = w.get(k2, 0) + v2
                    else:
                        stats[k] = stats.get(k, 0) + v
    why = stats.pop('why', {})
    print('g5:', stats)
    for k, v in sorted(why.items(), key=lambda x: -x[1])[:25]:
        print('  skip %6d  %s' % (v, k))


if __name__ == '__main__':
    main()
