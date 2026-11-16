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

# ---- [r23-g6c] G6 stage 3: call protocol (--proto N) ------------------------------------------
# --proto 1: every transformed function NAME becomes
#     extern "C" uint64_t NAME_g6(CPUContext& ctx, uint64_t G5ic_in)   (the body; returns inst_count)
#     void NAME(CPUContext& ctx) { ctx.inst_count = NAME_g6(ctx, ctx.inst_count); }   (wrapper: run_game,
#                                                   nwii_dispatch, HLE, untransformed callers, tables)
#   A direct call to a transformed callee passes G5ic in a register and takes the new value from the
#   return: no `ctx.inst_count = G5ic` before / `G5ic = ctx.inst_count` after. Invariants:
#   - the value a _g6 function returns is always the true inst_count (G5ic where the local is valid,
#     ctx.inst_count after an escape that may have changed it: poll slow path, dispatch, resume...);
#   - at a _g6 entry G5ic is dirty (ctx.inst_count may be stale: the caller did not write it), so the
#     forward analysis starts with it dirty and every escape writes it back first, as for any dirty
#     register; after a _g6 call G5ic is dirty in the caller, every other register clean (as before).
PROTO = 0
G6_FNS = set()        # functions that have a _g6 body (phase 2); empty in phase 1
G6_PASS = {}          # --proto 2: name -> guest GPRs passed in host registers (phase 1 computes them)
CLOSED_MAX = 4        # [r30-g6c3b] --closed-max N: at most N keys kept in host registers across one closed call
CLOSED = False        # [r30-g6c3b] --closed: calls to "closed" callees spill/reload only what the callee touches
G6_SUM = {}           # --closed: closed function -> (keys it may read, keys it may write), transitive (between phases)
G6_ARGREGS = [1, 3, 4, 5, 6, 7, 8, 9, 10]
G6_MAXARGS = 6        # x0 = ctx, x1 = inst_count, x2..x7 = guest registers (AAPCS64: 8 in registers)
G6_CALL = re.compile(r'^ctx\.pc = 0x([0-9A-F]+); (\w+)\(ctx\);$')
G6_TAIL = re.compile(r'^ctx\.pc = 0x([0-9A-F]+); if \(nwii::runtime::native_stack_ok\(\)\) (\w+)\(ctx\); (return;|goto (loc_[0-9A-F]+);)$')
RET_STMT = re.compile(r'\breturn;')

MARK = '// [r14-g5]'
FPR = False
EAGER_ENTRY = False  # --entry-fns: also transform nwii_entry functions (v1 did; worse)
LAZY = True  # --eager: reload everything live right after each escape (v1)
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
    # [r20-g6] local bctr after apply_g6_entry.py: jump-table switch, else nwii_dispatch (tail call)
    r'^ctx\.pc = ctx\.ctr; nwii_dispatch\(ctx\); return;$',
    # [r20-g6] blr of a callee inlined by apply_g6_inline.py: continue at the caller's check
    r'^ctx\.pc = (ctx\.lr|0x[0-9A-F]+); goto loc_[0-9A-F]+;$',
    # [r20-g6] tail call / tail bctr of an inlined callee: continue at the caller's check
    r'^ctx\.pc = 0x[0-9A-F]+; if \(nwii::runtime::native_stack_ok\(\)\) [\w:]+\(ctx\); goto loc_[0-9A-F]+;$',
    r'^ctx\.pc = ctx\.ctr; nwii_dispatch\(ctx\); goto loc_[0-9A-F]+;$',
)]
COND_OK = [re.compile(p) for p in (
    r'^if \([^;{}]*\) goto loc_[0-9A-F]+;$',
    r'^if \(ctx\.pc != 0x[0-9A-F]+\) return;$',
    r'^if \([^;{}]*\) \{ ctx\.pc = ctx\.lr; return; \}$',
    r'^if \(process_pending_callbacks\(ctx\)\) return;$',
    r'^if \(ctx\.ctr\) \{ ctx\.pc = ctx\.ctr; nwii_dispatch\(ctx\); if \(ctx\.pc != 0x[0-9A-F]+\) return; \} else nwii::runtime::note_null_call\(0x[0-9A-F]+, ctx\.lr\);$',
    r'^if \([^;{}]*\) \{ nwii::runtime::idle_wait\(ctx\); goto loc_[0-9A-F]+; \}$',
    r'^ctx\.pc = temp_lr; nwii_dispatch\(ctx\); if \(ctx\.pc != 0x[0-9A-F]+\) return;$',
    r'^if \([^;{}]*\) \{ ctx\.pc = (ctx\.lr|0x[0-9A-F]+); goto loc_[0-9A-F]+; \}$',  # [r20-g6] inlined conditional blr
    r'^switch \(ctx\.ctr\) \{( case 0x[0-9A-F]+: ctx\.pc = 0x[0-9A-F]+; goto loc_[0-9A-F]+;)+ \}$',  # [r20-g6] jump table
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
    if k == 'P': return 'G5pc'  # [r30-g6c3]
    if k == 'L': return 'G5lr'  # [r30-g6c3]
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
    if k == 'P': return 'ctx.pc'  # [r30-g6c3]
    if k == 'L': return 'ctx.lr'  # [r30-g6c3]
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
    if t == 'ctx.pc': return ('P',)  # [r30-g6c3]
    if t == 'ctx.lr': return ('L',)  # [r30-g6c3]
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


# ---- leaf inlining (--inline-leaves): _savegpr_N / _restgpr_N and the like -----------------
# A "simple leaf" is a generated function whose body (after its entry label) is only
# `ctx.inst_count += N;`, guest loads/stores between GPRs (`ctx.gpr[a] = mv_.readX<ID>(ctx.gpr[b] + k);`,
# `mv_.writeX<ID>(ctx.gpr[b] + k, ctx.gpr[a]);`), labels/comments, and `ctx.pc = ctx.lr; return;`,
# with an entry switch that only has the idle case and resume_pruned (nothing resumes inside it,
# no poll). A call `ctx.pc = 0xF; f(ctx);` + `if (ctx.pc != 0xR) return;` to it becomes its body
# followed by `ctx.pc = 0xR;` (what the callee's return leaves in ctx.pc): the same statements in
# the same order, and the caller's registers stay in locals across it. ~5 800 call sites (prologue
# / epilogue register save/restore of most game functions).
LEAVES = {}
LEAF_STMT = re.compile(r'^    (ctx\.inst_count \+= \d+;|ctx\.gpr\[\d+\] = mv_\.read(8|16|32)<\d+u>\(ctx\.gpr\[\d+\] \+ -?\d+\);'
                       r'|mv_\.write(8|16|32)<\d+u>\(ctx\.gpr\[\d+\] \+ -?\d+, ctx\.gpr\[\d+\]\);)( //.*)?$')
CALL_LINE = re.compile(r'^    ctx\.pc = 0x([0-9A-F]+); (\w+)\(ctx\);$')
CHECK_LINE = re.compile(r'^    if \(ctx\.pc != 0x([0-9A-F]+)\) return;$')


def collect_leaves(paths):
    for path in paths:
        src = open(path).read().split('\n')
        i = 0
        while i < len(src):
            m = re.match(r'^void (\w+)\(CPUContext& ctx\) \{$', src[i])
            if not m:
                i += 1; continue
            j = i + 1
            while src[j] != '}':
                j += 1
            fn = src[i + 1:j]
            i = j + 1
            name = m.group(1)
            try:
                k = fn.index('    switch (ctx.pc) {'.replace('    switch', '        switch'))
            except ValueError:
                continue
            sw = fn[k + 1:]
            if not (sw[0].strip() == 'case 0x0: return; // idle: no runnable thread' and
                    sw[1].strip() == 'default: nwii::runtime::resume_pruned(ctx); return;' and
                    sw[2] == '        }' and sw[3] == '    }' and LABEL.match(sw[4].strip())):
                continue
            body = sw[5:]
            if not body or body[-1] != '    ctx.pc = ctx.lr; return;':
                continue
            stm = []
            ok = True
            for l in body[:-1]:
                s_ = l.strip()
                if LABEL.match(s_) or s_.startswith('// 0x'):
                    continue
                if not LEAF_STMT.match(l):
                    ok = False; break
                stm.append(l)
            if ok and stm and len(stm) <= 40:
                LEAVES[name] = stm


def inline_leaves(body, stats):
    out, i = [], 0
    while i < len(body):
        m = CALL_LINE.match(body[i])
        if m and m.group(2) in LEAVES and i + 1 < len(body):
            c = CHECK_LINE.match(body[i + 1])
            if c:
                out.extend(LEAVES[m.group(2)])
                out.append('    ctx.pc = 0x%s; // [g5] inlined %s' % (c.group(1), m.group(2)))
                stats['inlined'] = stats.get('inlined', 0) + 1
                i += 2
                continue
        out.append(body[i]); i += 1
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


EXCLUDE = set()


def transform_function(lines, stats):
    if EXCLUDE:
        m = re.match(r'void (\w+)\(', lines[0])
        if m and m.group(1) in EXCLUDE:
            raise Skip('excluded (--exclude list)')
    body = lines[1:-1]
    if not any(REF.search(l) for l in body):
        stats['noref'] += 1
        return lines
    if LEAVES:
        body = inline_leaves(body, stats)
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
                    if PROTO == 3 and m.group(0) == 'ctx.pc':
                        continue  # [r30-g6c3] the entry switch reads ctx.pc (wrapper path only)
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
        nodes.append({'kind': 'line', 'lines': [l], 'cond': cflag[i], 'idx': i})
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
            if PROTO and not nd['cond']:
                m6 = G6_CALL.match(s) or G6_TAIL.match(s)
                if m6:
                    nd['callee'] = m6.group(2)
                if m6 and m6.group(2) in G6_FNS:
                    nd['g6'] = m6
                    if CLOSED and m6.re is G6_CALL and m6.group(2) in G6_SUM:
                        nd['cl'] = G6_SUM[m6.group(2)]
        else:
            nd['kind'] = 'pure'
            nd['ret'] = 'return' in code
            # [r30-g6c3] a return other than `ctx.pc = ctx.lr; return;` leaves ctx exact (spills everything)
            nd['xret'] = 'return' in code.replace('ctx.pc = ctx.lr; return;', '')
    fname = re.match(r'void (\w+)\(', lines[0]).group(1)
    if PROTO:
        keys.setdefault(('i',), None)
        if PROTO == 3:
            keys.setdefault(('P',), None); keys.setdefault(('L',), None)
        if PROTO == 2:
            # returned in registers: must exist as locals; so must every register passed to a callee
            keys.setdefault(('g', 1), None); keys.setdefault(('g', 3), None)
            for r_ in G6_PASS.get(fname, []):
                keys.setdefault(('g', r_), None)
            for nd in nodes:
                if 'g6' in nd:
                    for r_ in G6_PASS[nd['g6'].group(2)]:
                        keys.setdefault(('g', r_), None)
    keylist = sorted(keys, key=lambda k: (k[0], k[1:] if len(k) > 1 else ()))
    bit = {k: 1 << b for b, k in enumerate(keylist)}
    icbit = bit.get(('i',), 0)
    for nd in nodes:
        if 'cl' in nd:  # [r30-g6c3b] the callee's transitive read / write sets in this function's bits
            nd['clref'] = sum(bit[k_] for k_ in nd['cl'][0] if k_ in bit)
            nd['clmod'] = sum(bit[k_] for k_ in nd['cl'][1] if k_ in bit)
    def passbits(name):
        return sum(bit[('g', r_)] for r_ in G6_PASS.get(name, [])) if PROTO == 2 else 0
    RETB = 0  # [r23-g6c] registers a _g6 function hands back in host registers
    if PROTO:
        RETB = icbit | ((bit[('g', 1)] | bit[('g', 3)]) if PROTO == 2 else 0)
    if PROTO == 3:
        # [r30-g6c3] inst_count, pc and lr live in locals across direct calls / normal returns
        RETB = icbit | bit[('P',)] | bit[('L',)]
    ENTRY = (icbit | passbits(fname)) if PROTO else 0  # dirty at the _g6 entry (passed in registers)
    if PROTO == 3:
        ENTRY = RETB
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
        if PROTO and nd['ret']:
            # [r23-g6c] a return hands these back: they must be valid there. [r30-g6c3] not pc: a fast return
            # defines it (= lr), exact returns spill din (a real read of ctx.pc in the line stays a use)
            u |= (RETB & ~bit[('P',)]) if PROTO == 3 else RETB
        nd['def'] = d; nd['use'] = u
        hrw = icbit if nd['ic'] else 0
        if PROTO == 3 and 'write_dec(' in code:
            hrw |= bit[('P',)] | bit[('L',)]  # [r30-g6c3] write_dec prints pc/lr (NWII_SAMPLE)
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
    # [r23-g6c fix 10-nov] the entry switch's cases are entries too: a direct _g6 call to a function
    # whose own address is not its first one ("entry not first", e.g. func_801006C0 starts at
    # 0x800F0EB0) comes in through `case <own address>: goto loc_<own address>;` with G5ic (and, with
    # --proto 2, the passed registers) dirty. Before this fix only node 0 got ENTRY: the slow poll /
    # escapes reached from such a case did not write G5ic back (stale ctx.inst_count; difftest
    # 10-nov: func_808E20E0 & co. via func_801006C0_g6).
    entry_nodes = {0}
    if PROTO == 3 and nodes and nodes[0]['kind'] == 'switch':
        entry_nodes.add(label_idx['loc_' + own_addr(fname, nodes, label_idx)])
    if ENTRY:
        for nd in nodes:
            if nd['kind'] == 'switch':
                for b in nd['lines']:
                    mc = CASE_GOTO.match(b)
                    if mc and mc.group(2) in label_idx:
                        entry_nodes.add(label_idx[mc.group(2)])
    # [r30-g6c3b] two rounds when there are calls to closed callees: round 0 as c3 (everything written back /
    # reloaded around them); then each such call keeps in host registers up to CLOSED_MAX keys that are dirty,
    # live after the call and neither read nor written by the callee (nd["keep"]), and round 1 redoes the analysis.
    for rnd_ in range(2 if any('cl' in nd for nd in nodes) else 1):
        if rnd_ == 1:
            for k_, nd_ in enumerate(nodes):
                if 'cl' in nd_:
                    cand = din[k_] & lout[k_] & ~nd_['clref'] & ~nd_['clmod'] & ~RETB
                    nd_['keep'] = sum([bit[kk] for kk in keylist if cand & bit[kk]][:CLOSED_MAX])
        din = [0] * N; dout = [0] * N
        work = list(range(N)); inq = [True] * N
        while work:
            k = work.pop(); inq[k] = False
            x = ENTRY if k in entry_nodes else 0
            for p in pred[k]:
                x |= dout[p]
            din[k] = x
            nd = nodes[k]; kd = nd['kind']
            if kd == 'pure':
                o = (x if nd['cond'] else (x & ~nd['hrw'])) | nd['def']
            elif kd == 'esc':
                o = x if nd['cond'] else (RETB if 'g6' in nd else 0)
                if 'cl' in nd:  # [r30-g6c3b] the kept keys stay dirty (not written back), the rest as c3
                    o = (x & nd.get('keep', 0)) | RETB
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
                    if PROTO == 3 and not nd['xret']:
                        i_ |= din[k] & ~RETB  # [r30-g6c3] fast return: inst_count/lr handed back (in use), pc = lr
                    else:
                        i_ |= din[k]
            elif kd == 'esc':
                i_ = din[k]
                if 'g6' in nd and PROTO == 3:
                    # [r30-g6c3] pc is not written back before a direct call (the callee knows its address)
                    i_ = (din[k] & ~bit[('P',)]) | icbit | bit[('L',)]
                    if 'cl' in nd:  # [r30-g6c3b] kept keys are not reloaded: they flow through the call
                        i_ |= x & nd.get('keep', 0)
                elif 'g6' in nd:
                    i_ |= icbit | passbits(nd['g6'].group(2))  # passed to the callee
                    if not G6_CALL.match(nd['g6'].group(0)):
                        i_ |= RETB  # tail call: the no-native-stack path returns / goes on with them
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

    if 'nwii_entry' in label_idx and not EAGER_ENTRY:
        # local computed jump: every instruction is a resume label (merge point), so no local can
        # stay in a register; the locals only end up on the stack (measured on the console:
        # func_80250710 +25 %, func_8025DB30 +37 %, GXLoadTexObjPreLoaded +22 %). Left as r13.
        raise Skip('nwii_entry function (left unchanged)')
    if LAZY and 'nwii_entry' not in label_idx:
        return lazy_emit(lines, head, nodes, keylist, bit, pred, succ, din, lin, lout, label_idx, stats)

    RM = RETB  # [r23-g6c] handed back in registers: never spilled at returns
    RET_LOCAL = 'G5ic' if PROTO == 1 else 'G6RET(G5ic, G5r3, G5r1)'
    RET_CTX = 'ctx.inst_count' if PROTO == 1 else 'G6RET(ctx.inst_count, ctx.gpr[3], ctx.gpr[1])'
    def g6args(name):
        return ''.join(', G5r%d' % r_ for r_ in G6_PASS.get(name, [])) if PROTO == 2 else ''
    def g6take(callexpr):
        return ('G5ic = %s;' % callexpr) if PROTO == 1 else ('{ G6R t_ = %s; G6TAKE(t_); }' % callexpr)
    if PROTO:
        if FPR:
            raise SystemExit('--proto: no --fpr')
        if nodes and nodes[-1]['kind'] not in ('switch', 'label', 'raw') and not nodes[-1].get('uncond'):
            raise Skip('g6c: may fall off the end')
        if nodes and nodes[-1]['kind'] in ('label', 'raw', 'switch'):
            raise Skip('g6c: may fall off the end')
        if PROTO == 3:
            return emit_p3(lines, head, nodes, keylist, bit, din, lin, lout, label_idx, stats, fname, spill, reload, rename)
        if PROTO == 2 and G6_PHASE == 1:
            # guest GPRs live at the direct entry (label of the function's own address): passed in
            # host registers by direct callers (any choice is exact: callers and callee agree)
            ent = None
            mo = re.search(r'([0-9A-F]{8})$', fname)  # [r23-g6c fix 10-nov] own address (entry may not be first)
            if mo and 'loc_' + mo.group(1) in label_idx:
                ent = 'loc_' + mo.group(1)
            for nd in nodes:
                if ent is None and nd['kind'] == 'switch':
                    mm = re.match(r'\s*if \(ctx\.pc != 0x([0-9A-F]+)\) \{', nd['lines'][0])
                    ent = 'loc_' + mm.group(1) if mm else None
                    break
            live = lin[label_idx[ent]] if ent in label_idx else 0
            pr = [r_ for r_ in G6_ARGREGS if ('g', r_) in bit and live & bit[('g', r_)]][:G6_MAXARGS]
            stats.setdefault('g6pass', {})[fname] = pr
        out = ['extern "C" %s %s_g6(%s) {' % ('uint64_t' if PROTO == 1 else 'G6R', fname, g6params(fname, True))] + head
        def init(kk):
            if kk == ('i',): return 'G5ic_in'
            if PROTO == 2 and kk[0] == 'g' and kk[1] in G6_PASS.get(fname, []): return 'G6a%d' % kk[1]
            return load_expr(kk)
        out.append('    ' + ' '.join('%s %s = %s;' % (ctype(kk), lname(kk), init(kk)) for kk in keylist) + '  // [r14-g5] [r23-g6c]')
    else:
        out = [lines[0]] + head
        out.append('    ' + ' '.join('%s %s = %s;' % (ctype(kk), lname(kk), load_expr(kk)) for kk in keylist) + '  // [r14-g5]')
    for k, nd in enumerate(nodes):
        kd = nd['kind']
        if kd == 'switch' and PROTO:
            out.extend(RET_STMT.sub('return %s;' % RET_CTX, b) for b in nd['lines']); continue
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
            out.append('    if (NWII_UNLIKELY(nwii::runtime::poll_due(ctx))) { %sif (nwii::runtime::process_pending_callbacks_slow(ctx)) return%s; %s}'
                       % (spill(din[k]), (' ' + RET_CTX) if PROTO else '', reload(lout[k])))
            continue
        l = nd['lines'][0]
        ind = l[:len(l) - len(l.lstrip())]
        body_ = l.lstrip()
        if kd == 'esc' and 'g6' in nd:
            # [r23-g6c] direct call / tail call of a _g6 function: G5ic passed and returned
            stats['g6call'] = stats.get('g6call', 0) + 1
            m6 = nd['g6']
            a6, f6 = m6.group(1), m6.group(2)
            PB = passbits(f6)
            sp = spill(din[k] & ~icbit & ~PB)  # the passed ones are the callee's to write back
            rl = reload(lout[k] & ~RETB)
            sp2 = spill(din[k] & PB & ~RETB)   # no native stack: not called, write them back here
            ci = body_.find('//')
            cm_ = (' ' + body_[ci:]) if ci >= 0 else ''
            call = '%s_g6(ctx, G5ic%s)' % (f6, g6args(f6))
            if m6.re is G6_CALL:
                c_ = 'ctx.pc = 0x%s; %s%s' % (a6, g6take(call), (' ' + rl.rstrip()) if rl else '')
            elif m6.group(3) == 'return;':
                c_ = 'ctx.pc = 0x%s; if (nwii::runtime::native_stack_ok()) return %s; %sreturn %s;' % (a6, call, sp2, RET_LOCAL)
            else:
                c_ = 'ctx.pc = 0x%s; if (nwii::runtime::native_stack_ok()) %s%s %sgoto %s;' % (
                    a6, g6take(call), (' else { %s}' % sp2) if sp2 else '', rl, m6.group(4))
            out.append(ind + sp + c_ + cm_)
            continue
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
            if PROTO:
                c_ = RET_STMT.sub('return %s;' % RET_CTX, c_)  # after an escape ctx is the truth
            line = ind + sp + c_
            if not nd['uncond'] and rl:
                line += ' ' + rl.rstrip()
            out.append(line + cm_)
            continue
        code = rename(body_)
        if nd['ret'] and PROTO:
            ci = code.find('//')
            code = RET_STMT.sub('return %s;' % RET_LOCAL, code[:ci] if ci >= 0 else code) + (code[ci:] if ci >= 0 else '')
        if nd['ret'] and (din[k] & ~RM):
            sp = spill(din[k] & ~RM)
            if PROTO:
                code = code.replace('return %s;' % RET_LOCAL, 'return;')  # (put back below)
            if 'ctx.pc = ctx.lr; return;' in code:
                code = code.replace('ctx.pc = ctx.lr; return;', sp + 'ctx.pc = ctx.lr; return;', 1)
            elif re.match(r'^if \(ctx\.pc != 0x[0-9A-F]+\) return;', code):
                code = code.replace(') return;', ') { ' + sp + 'return; }', 1)
            elif code.startswith('return;'):
                code = sp + code
            else:
                raise Skip('pure return form: ' + code[:50])
            if PROTO:
                ci = code.find('//')
                code = RET_STMT.sub('return %s;' % RET_LOCAL, code[:ci] if ci >= 0 else code) + (code[ci:] if ci >= 0 else '')
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
    if PROTO == 1:
        out.append('void %s(CPUContext& ctx) { ctx.inst_count = %s_g6(ctx, ctx.inst_count); }  // [r23-g6c] wrapper' % (fname, fname))
    elif PROTO == 2:
        out.append('void %s(CPUContext& ctx) { G6R t_ = %s_g6(ctx, ctx.inst_count%s); ctx.inst_count = t_.ic; ctx.gpr[3] = (uint32_t)t_.r; ctx.gpr[1] = (uint32_t)(t_.r >> 32); }  // [r23-g6c] wrapper'
                   % (fname, fname, ''.join(', ctx.gpr[%d]' % r_ for r_ in G6_PASS.get(fname, []))))
    if PROTO:
        stats.setdefault('g6fns', []).append(fname)
    stats['done'] += 1
    return out


# ---- [r30-g6c3] --proto 3: inst_count, pc and lr in locals across direct calls ---------------------
# Every transformed function F becomes
#     extern "C" uint64_t F_g6(CPUContext& ctx, uint64_t G5ic_in, uint64_t G6lr0)
#     void F(CPUContext& ctx) { F_g6(ctx, ctx.inst_count, ctx.lr | G6W | G6CHK); }      (tail call)
# G6lr0 = the guest LR at entry (bits 0-31) | G6W (bit 32: "entered from outside: leave ctx exact at
# every return") | G6CHK (bit 33: "ctx.pc says where to start: run the entry switch").
# Keys of the G5 analysis: besides gpr/cr/xer/ctr/inst_count, ctx.pc (G5pc) and ctx.lr (G5lr).
#   - direct call `ctx.lr = R; ctx.pc = F; F(ctx); if (ctx.pc != R) return;` ->
#     `G5lr = R; t = F_g6(ctx, G5ic, G5lr); if (t < 0) return g6_exact(..); G5ic = t; G5pc = G5lr;`
#     (no ctx.pc/ctx.lr/ctx.inst_count stores before, no loads/compare after; the check folds away);
#   - a direct entry starts at the function's own address with G5pc = F, G5lr = lr0, G5ic = ic (all
#     three dirty: ctx may be stale; any escape writes them back first, as for any dirty register);
#   - `ctx.pc = ctx.lr; return;` ("fast" return): every dirty register except inst_count/pc/lr is
#     written back; then `return G6RETF(G5ic, G5lr)`: if G5lr == G6lr0 (same LR, not from outside) the
#     callee returns ic with bit 63 clear ("normal": the caller knows pc = lr = R); otherwise
#     g6_slowret writes ctx.inst_count/pc/lr (ctx exact) and returns bit 63 = (lr != lr0);
#   - every other return (after escapes, `if (ctx.pc != R) return;` after an untransformed call, the
#     entry switch, the poll) leaves ctx exact (all dirty written back) and returns g6_exact():
#     bit 63 clear iff ctx.pc == ctx.lr == lr0 (a caller in direct mode then goes on with pc = lr = R);
#   - bit 63 set: ctx is exact, the caller returns g6_exact() too (what `if (ctx.pc != R) return;` did);
#   - pc set before the interrupt poll (`ctx.pc = X;`) is now G5pc = X: written only on the slow path.
# Exactness: at every point where something outside the function can look at ctx (escapes, returns
# to a non-direct caller) ctx holds what e2 had there; between those points pc/lr/inst_count live in
# locals like the other registers.
G6CHK_TXT = 'G6CHK'


def own_addr(fname, nodes, label_idx):
    """the address a direct call enters at: the name's (func_XXXXXXXX), else (tests) the entry switch's first."""
    mo = re.search(r'([0-9A-F]{8})$', fname)
    if mo:
        a = mo.group(1)
    else:
        m = nodes and nodes[0]['kind'] == 'switch' and re.match(r'\s*if \(ctx\.pc != 0x([0-9A-F]+)\) \{', nodes[0]['lines'][0])
        if not m:
            raise Skip('g6c3: no own address')
        a = m.group(1)
    if 'loc_' + a not in label_idx:
        raise Skip('g6c3: no label for the own address')
    return a


def emit_p3(lines, head, nodes, keylist, bit, din, lin, lout, label_idx, stats, fname, spill, reload, rename):
    icb, Pb, Lb = bit[('i',)], bit[('P',)], bit[('L',)]
    R3 = icb | Pb | Lb
    EXACT = 'return g6_exact(ctx, G6lr0);'
    sw = [nd for nd in nodes if nd['kind'] == 'switch']
    if len(sw) > 1 or (sw and nodes[0]['kind'] != 'switch'):
        raise Skip('g6c3: entry switch not first / not one')
    if sw:
        own = own_addr(fname, nodes, label_idx)
        first = re.match(r'\s*if \(ctx\.pc != 0x([0-9A-F]+)\) \{', sw[0]['lines'][0])
        if not first:
            raise Skip('g6c3: entry switch form')
        first = first.group(1)
    else:
        # no entry switch: e2 runs from the top whatever ctx.pc is (direct or not); pc there = the caller's
        first = None
        lb = [nd for nd in nodes if nd['kind'] == 'label']
        if not lb or lb[0] is not nodes[0]:
            raise Skip('g6c3: no switch and no first label')
        own = lb[0]['name'][4:]

    def init(kk):
        if kk == ('i',): return 'G5ic_in'
        if kk == ('L',): return '(uint32_t)G6lr0'
        if kk == ('P',): return '0x%su' % own
        return load_expr(kk)
    out = ['extern "C" uint64_t %s_g6(nwii::runtime::CPUContext& ctx, uint64_t G5ic_in, uint64_t G6lr0) {' % fname] + head
    out.append('    ' + ' '.join('%s %s = %s;' % (ctype(kk), lname(kk), init(kk)) for kk in keylist) + '  // [r14-g5] [r30-g6c3]')
    if first is None:
        out.append('    if (NWII_UNLIKELY(G6lr0 & G6CHK)) G5pc = ctx.pc;  // [r30-g6c3] entered from outside, no entry switch')
    for k, nd in enumerate(nodes):
        kd = nd['kind']
        if kd == 'switch':
            out.append('    if (NWII_UNLIKELY(G6lr0 & G6CHK)) {  // [r30-g6c3] entered from outside: ctx.pc says where')
            out.append('    G5pc = ctx.pc;')
            out.extend(RET_STMT.sub(EXACT, b) for b in nd['lines'])
            out.append('    }' + (' else goto loc_%s;' % own if own != first else ''))
            continue
        if kd == 'label':
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
            # poll_due() reads ctx.pc (0 / 0xFFFFFFFC: dispatcher cases). Right after `ctx.pc = <code address>;`
            # (8 615 of the 8 923 polls) that test is false: g6_poll_due = poll_due without it. Else pc first.
            prev = nodes[k - 1] if k > 0 else None
            mpc = prev is not None and prev['kind'] == 'pure' and re.fullmatch(r'ctx\.pc = 0x([0-9A-F]+);', strip_comment(prev['lines'][0]).strip())
            if mpc and ((int(mpc.group(1), 16) + 4) & 0xFFFFFFFF) > 4:  # poll_due's test `pc + 4u <= 4u` is false
                due = 'g6_poll_due(ctx)'
                stats['poll_k'] = stats.get('poll_k', 0) + 1
            else:
                due = '(%snwii::runtime::poll_due(ctx))' % ('ctx.pc = G5pc, ' if din[k] & Pb else '')
            out.append('    if (NWII_UNLIKELY(%s)) { %sif (nwii::runtime::process_pending_callbacks_slow(ctx)) %s %s}'
                       % (due, spill(din[k]), EXACT, reload(lout[k])))
            continue
        l = nd['lines'][0]
        ind = l[:len(l) - len(l.lstrip())]
        body_ = l.lstrip()
        ci = body_.find('//')
        cm_ = (' ' + body_[ci:]) if ci >= 0 else ''
        if kd == 'esc' and 'g6' in nd:
            stats['g6call'] = stats.get('g6call', 0) + 1
            m6 = nd['g6']
            a6, f6 = m6.group(1), m6.group(2)
            sp = spill(din[k] & ~R3)
            rl = reload(lout[k] & ~R3)
            bitsp = ''
            if 'cl' in nd:
                # [r30-g6c3b] closed callee (no escape, transitively): write back only the dirty registers it may
                # read, reload only what it may write; the rest stays in locals across the call. If it does not
                # return normally (only g6_slowret / no native stack), write the rest back before unwinding.
                stats['closedcall'] = stats.get('closedcall', 0) + 1
                kp = nd.get('keep', 0)
                sp = spill(din[k] & ~kp & ~R3)
                rl = reload(lout[k] & ~kp & ~R3)
                bitsp = spill(din[k] & kp & ~R3)
                if kp:
                    stats['closedkept'] = stats.get('closedkept', 0) + bin(kp).count('1')
            if m6.re is G6_CALL:
                c_ = ('{ uint64_t t_ = %s_g6(ctx, G5ic, G5lr); if (NWII_UNLIKELY((int64_t)t_ < 0)) %s G5ic = t_; } G5pc = G5lr;%s'
                      % (f6, ('{ %s%s }' % (bitsp, EXACT)) if bitsp else EXACT, (' ' + rl.rstrip()) if rl else ''))
            elif m6.group(3) == 'return;':
                c_ = ('if (nwii::runtime::native_stack_ok()) { if (G5lr == (uint32_t)G6lr0) return %s_g6(ctx, G5ic, G6lr0 & ~G6CHK); '
                      'return g6_tailfix(ctx, %s_g6(ctx, G5ic, G5lr), G5lr, G6lr0); } %sctx.pc = 0x%s; %s'
                      % (f6, f6, spill(din[k] & (icb | Lb)), a6, EXACT))
            else:
                c_ = ('G5pc = 0x%s; if (nwii::runtime::native_stack_ok()) { uint64_t t_ = %s_g6(ctx, G5ic, G5lr); '
                      'if (NWII_UNLIKELY((int64_t)t_ < 0)) %s G5ic = t_; G5pc = G5lr; } %sgoto %s;'
                      % (a6, f6, EXACT, rl, m6.group(4)))
            out.append(ind + sp + c_ + cm_)
            continue
        if kd == 'esc':
            stats['esc'] += 1
            sp = spill(din[k])
            rl = reload(lout[k])
            if nd['reload_before_goto'] and rl:
                gm2 = re.search(r'goto (loc_[0-9A-F]+);', body_)
                if body_[:gm2.start()].rstrip().endswith(')'):
                    body_ = body_[:gm2.start()] + '{ ' + rl + gm2.group(0) + ' }' + body_[gm2.end():]
                else:
                    body_ = body_[:gm2.start()] + rl + body_[gm2.start():]
            ci = body_.find('//')
            c_, cm_ = (body_[:ci].rstrip(), ' ' + body_[ci:]) if ci >= 0 else (body_, '')
            c_ = RET_STMT.sub(EXACT, c_)  # after an escape ctx is the truth
            line = ind + sp + c_
            if not nd['uncond'] and rl:
                line += ' ' + rl.rstrip()
            out.append(line + cm_)
            continue
        code = rename(body_)
        ci = code.find('//')
        c_, cm_ = (code[:ci].rstrip(), ' ' + code[ci:]) if ci >= 0 else (code, '')
        if nd['ret']:
            FAST = 'G5pc = G5lr; return;'
            nfast = c_.count(FAST)
            if nfast:
                c_ = c_.replace(FAST, spill(din[k] & ~R3) + 'return G6RETF(G5ic, G5lr);')
                stats['fastret'] = stats.get('fastret', 0) + nfast
            spx = spill(din[k])
            n0 = len(RET_STMT.findall(c_))
            c_ = RET_STMT.sub(lambda m: '{ ' + spx + EXACT + ' }', c_)
            if n0:
                stats['exactret'] = stats.get('exactret', 0) + n0
            if not nfast and not n0:
                raise Skip('g6c3: return form ' + c_[:50])
        if nd['hrw'] or nd['hw']:
            sp = spill(nd['hrw'] & din[k])
            rl = reload(nd['hw'] & lout[k])
            if sp or rl:
                stats['partial'] += 1
            c_ = sp + c_ + ((' ' + rl.rstrip()) if rl else '')
        out.append(ind + c_ + cm_ if c_ else ind + cm_.lstrip())
    out.append(lines[-1])
    out.append('void %s(CPUContext& ctx) { %s_g6(ctx, ctx.inst_count, (uint64_t)ctx.lr | G6W | G6CHK); }  // [r30-g6c3] wrapper' % (fname, fname))
    # [r30-g6c3b] summary for --closed: closed locally = nothing in it can look at ctx (no poll, no raw block,
    # no MMIO / clock reader, every escape a direct (tail) call to a _g6 function); keys read = all its keys
    # (all are loaded at the entry), keys written = the ones some statement defines.
    loc = all(nd['kind'] not in ('poll', 'raw') for nd in nodes) and \
        all('callee' in nd and not nd['cond'] for nd in nodes if nd['kind'] == 'esc') and \
        not any(nd['kind'] == 'pure' and (nd.get('ic') or nd.get('hrw') or nd.get('hw')) for nd in nodes)
    dm = 0
    for nd in nodes:
        if nd['kind'] == 'pure':
            dm |= nd['def']
    stats.setdefault('g6sum', {})[fname] = (
        loc, frozenset(kk for kk in keylist if kk[0] not in 'iPL'),
        frozenset(kk for kk in keylist if kk[0] not in 'iPL' and dm & bit[kk]),
        frozenset(nd['callee'] for nd in nodes if nd['kind'] == 'esc' and 'callee' in nd))
    stats.setdefault('g6fns', []).append(fname)
    stats['done'] += 1
    return out


def lazy_emit(lines, head, nodes, keylist, bit, pred, succ, din, lin, lout, label_idx, stats):
    """Lazy reload (default): after an escape the registers stay in ctx ("stale") and each one is
    reloaded just before its first use (at a top-level statement; a multi-line block reloads
    everything it touches before its first line), instead of reloading everything live right
    after the call (which kept dozens of values alive across the next calls: stack spills).
    A key is never stale on one path and modified-but-not-written-back on another at the same
    point: at a label, the live keys that may be dirty on some incoming edge are reloaded on the
    incoming edges where they may be stale (fallthrough: before the label; goto: inside braces
    before the goto; entry switch: in the case). Dirty implies valid, so write-backs stay as in
    the eager scheme. The cold slow-poll path and nested (conditional) escapes reload eagerly."""
    N = len(nodes)
    ALL = (1 << len(keylist)) - 1
    # top-level regions: a line at indent 4 that leaves braces open, up to where they close
    region = [None] * N
    k = 0
    while k < N:
        nd = nodes[k]
        if nd['kind'] in ('pure', 'esc') and 'idx' in nd:
            l = nd['lines'][0]
            c = strip_comment(l)
            if l.startswith('    ') and not l.startswith('     ') and c.count('{') > c.count('}'):
                depth = c.count('{') - c.count('}')
                j = k + 1
                while j < N and depth > 0:
                    cj = strip_comment(nodes[j]['lines'][0]) if nodes[j]['kind'] not in ('switch', 'raw') else ''
                    if nodes[j]['kind'] in ('label', 'switch', 'raw'):
                        raise Skip('lazy: label/raw inside a block')
                    depth += cj.count('{') - cj.count('}')
                    j += 1
                for t in range(k, j):
                    region[t] = k
                k = j
                continue
        k += 1
    # uses (incl. defs: reloading a key only defined is a dead load) per region / single node
    def touched(nd):
        return nd.get('use', 0) | nd.get('def', 0)
    ureg = {}
    for t in range(N):
        if region[t] is not None and nodes[t]['kind'] == 'pure':
            ureg[region[t]] = ureg.get(region[t], 0) | touched(nodes[t])
    # forward stale analysis. At a label a live key is stale only if it is stale on every incoming
    # edge (must-stale); where it is valid on some edge, it is reloaded on the stale edges (also
    # what keeps loop-invariant values in registers: loaded once before the loop). Dead keys:
    # may-stale. Decreasing iteration from "all stale".
    sin = [ALL] * N; sout = [ALL] * N
    def transfer(t, x):
        nd = nodes[t]; kd = nd['kind']
        if kd == 'switch':
            return ALL
        if kd == 'label':
            return x
        if kd in ('raw', 'esc'):
            if kd == 'esc' and nd['cond']:
                return x | (ALL & ~lout[t])  # nested escape: live keys reloaded eagerly after it
            return ALL
        if kd == 'poll':
            return x
        r = region[t]
        if r is not None:
            if t == r:
                return x & ~ureg.get(r, 0)
            return x  # inside a block: everything it touches was made valid at its head
        return x & ~(touched(nd))
    changed = True
    while changed:
        changed = False
        for t in range(N):
            if nodes[t]['kind'] == 'label':
                a, o = ALL, 0
                for p in pred[t]:
                    a &= sout[p]; o |= sout[p]
                x = (a & lin[t]) | (o & ~lin[t]) if pred[t] else ALL
            elif t == 0 or nodes[t]['kind'] == 'switch' or not pred[t]:
                x = ALL
            else:
                x = 0
                for p in pred[t]:
                    x |= sout[p]
            o2 = transfer(t, x)
            if x != sin[t] or o2 != sout[t]:
                sin[t] = x; sout[t] = o2; changed = True
    # reloads on the edge p -> label t: live at t, valid at t, stale on this edge
    need = [0] * N
    for t in range(N):
        if nodes[t]['kind'] == 'label':
            need[t] = lin[t] & ~sin[t]

    def spill(mask):
        return ''.join(store_stmt(kk) + ' ' for kk in keylist if mask & bit[kk])

    def reload(mask):
        return ''.join('%s = %s; ' % (lname(kk), load_expr(kk)) for kk in keylist if mask & bit[kk])

    def rename(code):
        return REF.sub(lambda m: lname(mkey(m)), code)

    def edge_gotos(code, state):
        # goto L -> { reloads goto L; } for the keys L needs that may be stale on this edge
        def f(m):
            L = m.group(1)
            if L == 'nwii_entry':
                raise Skip('lazy: nwii_entry')
            rl = reload(need[label_idx[L]] & state)
            if not rl:
                return m.group(0)
            stats['edge'] = stats.get('edge', 0) + 1
            return '{ ' + rl + m.group(0) + ' }'
        return re.sub(r'goto (loc_[0-9A-F]+|nwii_entry);', f, code)

    out = [lines[0]] + head
    out.append('    ' + ' '.join('%s %s = 0;' % (ctype(kk), lname(kk)) for kk in keylist) + '  // [r14-g5] lazy')
    for t, nd in enumerate(nodes):
        kd = nd['kind']
        if kd == 'switch':
            for b in nd['lines']:
                m = CASE_GOTO.match(b)
                if m:
                    rl = reload(need[label_idx[m.group(2)]] & ALL)
                    if rl:
                        b = '            case 0x%s: { %sgoto %s; }' % (m.group(1), rl, m.group(2))
                out.append(b)
            continue
        if kd == 'label':
            if t - 1 in pred[t] and t > 0:
                rl = reload(need[t] & sout[t - 1])
                if rl:
                    out.append('    ' + rl.rstrip() + '  // [g5] edge')
                    stats['edge'] = stats.get('edge', 0) + 1
            out.extend(nd['lines'])
            continue
        if kd in ('raw', 'esc') and any('xer_ca' in b for b in nd['lines']):
            raise Skip('xer_ca in raw/esc ' + nd['lines'][0][:60])
        if kd == 'raw':
            sp = spill(din[t])
            if sp: out.append('    ' + sp.rstrip())
            out.extend(nd['lines'])
            stats['raw'] += 1
            continue
        if kd == 'poll':
            stats['poll'] += 1
            out.append('    if (NWII_UNLIKELY(nwii::runtime::poll_due(ctx))) { %sif (nwii::runtime::process_pending_callbacks_slow(ctx)) return; %s}'
                       % (spill(din[t]), reload(lout[t] & ~sin[t])))
            continue
        l = nd['lines'][0]
        ind = l[:len(l) - len(l.lstrip())]
        body_ = l.lstrip()
        if kd == 'esc':
            stats['esc'] += 1
            sp = spill(din[t])
            if nd['goto_bypasses']:
                raise Skip('lazy: goto before escape')
            if nd['gotos']:
                if not nd['reload_before_goto']:
                    raise Skip('lazy: esc goto form')
                body_ = edge_gotos(body_, ALL)
            ci = body_.find('//')
            c_, cm_ = (body_[:ci].rstrip(), ' ' + body_[ci:]) if ci >= 0 else (body_, '')
            line = ind + sp + c_
            if nd['cond']:
                rl = reload(lout[t])
                if rl:
                    line += ' ' + rl.rstrip()
            out.append(line + cm_)
            continue
        # pure
        r = region[t]
        pre = ''
        if r is None:
            pre = reload(touched(nd) & sin[t])
        elif t == r:
            pre = reload(ureg.get(r, 0) & sin[t])
        if pre:
            stats['lazy'] = stats.get('lazy', 0) + 1
            out.append(ind + pre.rstrip())
        code = rename(body_)
        if nd['gotos']:
            code = edge_gotos(code, sout[t])
        if nd['ret'] and din[t]:
            sp = spill(din[t])
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
            if nd['hw']:
                raise Skip('lazy: partial write (fpr)')
            ci = code.find('//')
            c_, cm_ = (code[:ci].rstrip(), ' ' + code[ci:]) if ci >= 0 else (code, '')
            sp = spill(nd['hrw'] & din[t])
            if sp:
                stats['partial'] += 1
            code = sp + c_ + cm_
        out.append(ind + code)
    out.append(lines[-1])
    stats['done'] += 1
    return out


def dout_of(nodes, p, din):
    """may-dirty after node p (same transfer as the forward analysis)."""
    nd = nodes[p]; kd = nd['kind']; x = din[p]
    if kd == 'pure':
        return (x if nd['cond'] else (x & ~nd['hrw'])) | nd['def']
    if kd == 'esc':
        return x if nd['cond'] else 0
    if kd == 'raw':
        return 0
    return x


CASE_GOTO = re.compile(r'^            case 0x([0-9A-F]+): goto (loc_[0-9A-F]+);$')


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


def scan_file(path):
    """[r23-g6c] phase 1: which functions of this file get a _g6 body (nothing written)."""
    st = {'done': 0, 'skip': 0, 'noref': 0, 'esc': 0, 'raw': 0, 'retspill': 0, 'poll': 0, 'partial': 0}
    process_text(open(path).read().split('\n'), st)
    return st


G6_PHASE = 0


def g6params(name, body=False):
    ps = ['nwii::runtime::CPUContext& ctx', 'uint64_t G5ic_in' if body else 'uint64_t ic']
    if PROTO == 2:
        ps += ['uint32_t G6a%d' % r_ for r_ in G6_PASS.get(name, [])]
    return ', '.join(ps)


def close_sums(raw):
    """[r30-g6c3b] closed functions (locally closed, every direct callee transformed and closed; greatest fixpoint)
    and their transitive read/write key sets."""
    closed = {n for n, v in raw.items() if v[0] and n in G6_FNS}
    ch = True
    while ch:
        ch = False
        for n in list(closed):
            if any(c not in closed for c in raw[n][3]):
                closed.discard(n); ch = True
    ref = {n: set(raw[n][1]) for n in closed}; mod = {n: set(raw[n][2]) for n in closed}
    ch = True
    while ch:
        ch = False
        for n in closed:
            for c in raw[n][3]:
                if not ref[c] <= ref[n]: ref[n] |= ref[c]; ch = True
                if not mod[c] <= mod[n]: mod[n] |= mod[c]; ch = True
    return {n: (frozenset(ref[n]), frozenset(mod[n])) for n in closed}


def decl(name):
    if PROTO == 3:
        return 'extern "C" uint64_t %s_g6(nwii::runtime::CPUContext& ctx, uint64_t ic, uint64_t lr0x);' % name
    return 'extern "C" %s %s_g6(%s);' % ('uint64_t' if PROTO == 1 else 'G6R', name, g6params(name))


def decl_head():
    h = ['// [r23-g6c] _g6 bodies (r9/r23g6c/tools/apply_g6c.py --proto %d)' % PROTO]
    if PROTO == 2:
        h += ['struct G6R { uint64_t ic; uint64_t r; };  // r = guest r3 | guest r1 << 32 (x0, x1)',
              '#define G6RET(ic_, r3_, r1_) G6R{(uint64_t)(ic_), (uint64_t)(uint32_t)(r3_) | ((uint64_t)(uint32_t)(r1_) << 32)}',
              '#define G6TAKE(t_) (G5ic = (t_).ic, G5r3 = (uint32_t)(t_).r, G5r1 = (uint32_t)((t_).r >> 32))']
    if PROTO == 3:
        h += ['// [r30-g6c3] call protocol 3 (r9/r30g6c3/tools/apply_g6c.py): see emit_p3 there',
              '#define G6BIT (1ull << 63)  // returned: "not a normal return to lr0; ctx is exact"',
              '#define G6W (1ull << 32)    // lr0x: entered from outside (wrapper): leave ctx exact at returns',
              '#define G6CHK (1ull << 33)  // lr0x: run the entry switch on ctx.pc',
              '#define G6RETF(ic_, lr_) ((uint64_t)(lr_) == G6lr0 ? (uint64_t)(ic_) : g6_slowret(ctx, (ic_), (lr_), G6lr0))',
              '// poll_due (runtime/cpu_context.h, r14g5 = r7f) without its ctx.pc test, for polls right after',
              '// `ctx.pc = <code address>;` (that test is false there). Keep in sync with poll_due.',
              '__attribute__((always_inline, unused)) static inline bool g6_poll_due(nwii::runtime::CPUContext& ctx) {',
              '    const int32_t left = ctx.poll_left.load(std::memory_order_relaxed) - 1;',
              '    ctx.poll_left.store(left, std::memory_order_relaxed);',
              '    if (NWII_UNLIKELY(left <= 0)) return true;',
              '    if (!(ctx.msr & 0x8000)) return false;',
              '    if (NWII_UNLIKELY(ctx.poll_dec_due | ((nwii::runtime::hw::pi_intsr.load(std::memory_order_relaxed) &',
              '                      nwii::runtime::hw::pi_intmr.load(std::memory_order_relaxed)) != 0) |',
              '                      (ctx.pending_cb_count.load(std::memory_order_relaxed) != 0)))',
              '        return true;',
              '    return false;',
              '}',
              '__attribute__((noinline, unused)) static uint64_t g6_exact(nwii::runtime::CPUContext& ctx, uint64_t lr0x) {',
              '    const uint64_t ic = ctx.inst_count;',
              '    return (ctx.pc == (uint32_t)lr0x && ctx.lr == (uint32_t)lr0x) ? ic : (ic | G6BIT);',
              '}',
              '__attribute__((noinline, unused)) static uint64_t g6_slowret(nwii::runtime::CPUContext& ctx, uint64_t ic, uint32_t lr, uint64_t lr0x) {',
              '    ctx.inst_count = ic; ctx.pc = lr; ctx.lr = lr;',
              '    return lr == (uint32_t)lr0x ? ic : (ic | G6BIT);',
              '}',
              '__attribute__((noinline, unused)) static uint64_t g6_tailfix(nwii::runtime::CPUContext& ctx, uint64_t t, uint32_t lr, uint64_t lr0x) {',
              '    if ((int64_t)t >= 0) { ctx.inst_count = t; ctx.pc = lr; ctx.lr = lr; }',
              '    return g6_exact(ctx, lr0x);',
              '}']
    return h


def one_file(path):
    st = {'done': 0, 'skip': 0, 'noref': 0, 'esc': 0, 'raw': 0, 'retspill': 0, 'poll': 0, 'partial': 0}
    src = open(path).read().split('\n')
    res = [MARK + (' guest registers in locals%s%s%s (r9/r14g5/tools/apply_g5_regs.py)' % (' + FPR' if FPR else '', '' if UNROLL else ', no unroll', ', lazy reload' if LAZY else ', eager reload') + (', %d excluded' % len(EXCLUDE) if EXCLUDE else '') + (', %d leaves inlined' % len(LEAVES) if LEAVES else ''))]
    if PROTO:
        res[0] += ' [r23-g6c] call protocol %d (r9/r23g6c/tools/apply_g6c.py)' % PROTO
        k = src.index('#include "functions.h"')
        src = src[:k + 1] + ['#include "g6c_decls.h"  // [r23-g6c]'] + src[k + 1:]
    res += process_text(src, st)
    tmp = path + '.tmp'
    with open(tmp, 'w') as f:
        f.write('\n'.join(res))
    os.replace(tmp, path)
    return st


def main():
    global FPR, REF
    a = sys.argv[1:]
    global UNROLL, LAZY, EAGER_ENTRY
    if '--exclude' in a:
        k = a.index('--exclude')
        EXCLUDE.update(x.strip() for x in open(a[k + 1]) if x.strip() and not x.startswith('#'))
        del a[k:k + 2]
    inl = '--inline-leaves' in a
    if inl:
        a.remove('--inline-leaves')
    if '--entry-fns' in a:
        EAGER_ENTRY = True; a.remove('--entry-fns')
    if '--eager' in a:
        LAZY = False; a.remove('--eager')
    global PROTO, G6_PHASE
    if '--proto' in a:
        k = a.index('--proto'); PROTO = int(a[k + 1]); del a[k:k + 2]
    global CLOSED
    if '--closed' in a:
        CLOSED = True; a.remove('--closed')
    global CLOSED_MAX
    if '--closed-max' in a:
        k = a.index('--closed-max'); CLOSED_MAX = int(a[k + 1]); del a[k:k + 2]
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
    if inl:
        k = a.index('--leaves-from') if '--leaves-from' in a else -1
        ldir = a[k + 1] if k >= 0 else None
        if k >= 0: del a[k:k + 2]
        if ldir is None:
            sys.exit('--inline-leaves needs --leaves-from <original export dir>')
        collect_leaves(sorted(os.path.join(ldir, f) for f in os.listdir(ldir) if re.fullmatch(r'output_\d+\.cpp', f)))
        print('leaves:', len(LEAVES), sorted(LEAVES)[:12])
    if PROTO and LAZY:
        sys.exit('--proto needs --eager')
    if PROTO == 3:
        REF = re.compile(REF_BASE + r'|ctx\.pc\b|ctx\.lr\b')  # [r30-g6c3]
    if a[0] == '--file':
        src = open(a[1]).read().split('\n')
        pre = []
        if PROTO:

            G6_PHASE = 1
            st1 = dict(stats)
            process_text(src, st1)
            G6_FNS.update(st1.get('g6fns', []))
            G6_PASS.update(st1.get('g6pass', {}))
            if CLOSED:
                G6_SUM.update(close_sums(st1.get('g6sum', {})))
                print('g6c3b: %d closed functions' % len(G6_SUM))
            G6_PHASE = 2
            pre = decl_head() + [decl(x) for x in sorted(G6_FNS)]
        res = process_text(src, stats)
        if PROTO and set(stats.get('g6fns', [])) != G6_FNS:
            sys.exit('g6c: phase 2 transformed another set of functions')
        open(a[2], 'w').write('\n'.join(pre + res))
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
        if PROTO:

            G6_PHASE = 1
            raw = {}
            with multiprocessing.get_context('fork').Pool(int(os.environ.get('G5_JOBS', '4'))) as pool:
                for st in pool.map(scan_file, todo):
                    G6_FNS.update(st.get('g6fns', []))
                    G6_PASS.update(st.get('g6pass', {}))
                    raw.update(st.get('g6sum', {}))
            if CLOSED:
                G6_SUM.update(close_sums(raw))
                with open(os.path.join(exp, 'g6c_closed.txt'), 'w') as f:
                    for n_ in sorted(G6_SUM):
                        f.write('%s r:%d w:%d\n' % (n_, len(G6_SUM[n_][0]), len(G6_SUM[n_][1])))
                print('g6c3b: %d closed functions' % len(G6_SUM))
            G6_PHASE = 2
            with open(os.path.join(exp, 'g6c_decls.h'), 'w') as f:
                f.write('#pragma once\n#include "runtime/cpu_context.h"\n')
                f.write('\n'.join(decl_head() + [decl(x) for x in sorted(G6_FNS)]) + '\n')
            with open(os.path.join(exp, 'g6c_fns.txt'), 'w') as f:
                f.write(''.join('%s %s\n' % (x, ','.join('r%d' % r_ for r_ in G6_PASS.get(x, []))) for x in sorted(G6_FNS)))
            print('g6c: %d functions with a _g6 body' % len(G6_FNS))
        # fork: the workers must inherit the option globals (forkserver/spawn re-import the module)
        with multiprocessing.get_context('fork').Pool(int(os.environ.get('G5_JOBS', '4'))) as pool:
            for st in pool.map(one_file, todo):
                for k, v in st.items():
                    if k == 'g6fns':
                        stats.setdefault('g6fns', []).extend(v)
                    elif k in ('g6pass', 'g6sum'):
                        pass
                    elif k == 'why':
                        w = stats.setdefault('why', {})
                        for k2, v2 in v.items(): w[k2] = w.get(k2, 0) + v2
                    else:
                        stats[k] = stats.get(k, 0) + v
    why = stats.pop('why', {})
    if PROTO and set(stats.pop('g6fns', [])) != G6_FNS:
        sys.exit('g6c: phase 2 transformed another set of functions')
    stats.pop('g6fns', None); stats.pop('g6pass', None); stats.pop('g6sum', None)
    print('g5:', stats)
    for k, v in sorted(why.items(), key=lambda x: -x[1])[:25]:
        print('  skip %6d  %s' % (v, k))


if __name__ == '__main__':
    main()
