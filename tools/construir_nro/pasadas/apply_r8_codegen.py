#!/usr/bin/env python3
"""apply_reglocals.py <export_dir> [--check]

R8 codegen post-pass (runs after apply_fastdispatch.py): keeps the guest
registers of every recompiled function in C++ locals instead of the
CPUContext struct, so the host compiler can hold them in host registers.

Why: every guest store goes through a uint8_t* (MMU fast path, memcpy +
bswap), which may alias anything, so the compiler had to reload ctx.gpr[]/
ctx.cr[] after every guest store and store every result back at once. With
locals (address never taken) nothing aliases them, and dead CR/XER updates
(cmp results nobody reads, "so" copies of XER) become dead stores the
compiler removes by itself.

Cached state: GPRs (ctx.gpr[N]), CR bits (ctx.cr[N].lt/gt/eq/so), XER, CTR.
Not cached: pc, lr, inst_count, FPRs (helpers index them), msr, ...

Exactness: ctx is made exact again ("spill") before every point where
anything outside the function can look at it, and locals are re-read
("reload") after every point where something outside may have changed it:
  - escapes: any statement that passes ctx to a function (guest calls,
    nwii_dispatch, process_pending_callbacks = interrupt delivery/longjmp,
    interpret_step, HLE handlers, idle_wait...), unknown ctx members, and
    multi-line blocks that index registers with a variable (lmw/stmw/mfcr/
    mtcrf loops), which keep their ctx text: spill before, reload after;
  - returns: spill before.
Spills store only registers that may be dirty there (forward may-analysis
over the function's goto CFG); reloads load only registers live afterwards
(backward liveness). Functions with any statement shape this pass does not
know are left unchanged (reported).

Switch pruning (same pass): each function starts with a switch that can resume
it at every instruction. Only addresses something can actually resume at keep
their case: every guest address the generated code stores in ctx.pc/ctx.lr/
ctx.srr0 or compares ctx.pc with (return addresses, interrupt poll points, tail
jumps into other functions, syscalls), plus every case of run_game's table
(function starts and the analyzer's data-referenced code addresses). Functions
with a local computed jump (nwii_entry) keep all cases. Any other address goes
to nwii::runtime::resume_pruned, which interprets one instruction (exact, only
slower) and logs it. With fewer entries, the labels stop being merge points, so
the host compiler can keep the register locals in host registers.
"""
import os, re, sys

MARK = '// [reglocals]'
ENTRIES = None  # set of guest addresses that may be resumed at (switch cases kept)
CASE = re.compile(r'^            case 0x([0-9A-F]+): goto loc_[0-9A-F]+;$')

REF_BASE = r'ctx\.gpr\[(\d+)\]|ctx\.cr\[(\d)\]\.(lt|gt|eq|so)|ctx\.xer\b|ctx\.ctr\b'
REF = re.compile(REF_BASE)
MEMVIEW = False  # --memview: guest memory accesses through a per-function MemView
PRUNE_ONLY = False  # --prune-only: only prune the entry switches, no register locals
FPR = False  # --fpr: also cache ctx.fpr[N] / ctx.ps1[N] (see rewrite_fp)
ASSIGN_AFTER = re.compile(r'\s*(=(?!=)|\+=|-=|\*=|/=|\|=|&=|\^=|<<=|>>=|\+\+|--)')
ESCAPE = re.compile(r'\bctx\s*[,)]')
LABEL = re.compile(r'^(loc_[0-9A-F]+|nwii_entry): ;$')
GOTO = re.compile(r'goto (loc_[0-9A-F]+|nwii_entry);')
CTX_MEMBER = re.compile(r'\bctx\.([a-z_0-9]+)')
# ctx members that generated code may touch without leaving the function:
# plain fields not cached here, and helpers that only use fpr/ps1/fpscr/gqr/mmu.
SAFE_FIELDS = {'gpr', 'cr', 'xer', 'ctr', 'pc', 'lr', 'inst_count', 'mmu', 'fpr', 'ps1', 'gqr', 'msr',
               'srr0', 'srr1', 'hid2', 'dma_u', 'dma_l', 'sprg', 'fpscr', 'dispatch_saved_ctx',
               'reservation_addr'}
SAFE_HELPERS = {'lfs', 'stfs', 'fmuls', 'fsubs', 'fadds', 'psq_load', 'psq_store', 'fdivs', 'fctiwz',
                'fctiw', 'frsp', 'ps_muls0', 'ps_sub', 'ps_mul', 'ps_add', 'ps_madd', 'ps_sum0',
                'ps_madds0', 'ps_mr', 'fmadds', 'ps_merge00', 'ps_nmsub', 'ps_merge10', 'fres',
                'ps_msub', 'ps_neg', 'ps_madds1', 'ps_merge11', 'ps_merge01', 'ps_muls1', 'ps_abs',
                'fnmsubs', 'fnmadds', 'ps_sum1', 'frsqrte', 'fmsubs', 'mffs', 'write_hid2', 'write_dmal',
                'ps_nmadd', 'mtfsf', 'mtfsfi', 'dcbz', 'write_dec', 'read_dec', 'mtfsb', 'read_timebase',
                'ps_div', 'ps_res', 'ps_rsqrte', 'ps_sel', 'ps_nabs', 'fmadd', 'fmsub', 'fnmadd',
                'fnmsub', 'stfiwx', 'set_fpscr', 'fill', 'set_bits'}
FCMP = re.compile(r'ctx\.fcmp\((\d), (ctx\.(?:fpr|ps1)\[\d+\]), (ctx\.(?:fpr|ps1)\[\d+\])\);')
PSCMP = re.compile(r'ctx\.ps_cmp[uo]([01])\((\d), (\d+), (\d+)\);')
# Statement lines that end the straight-line flow.
UNCOND = [re.compile(p) for p in (
    r'^goto \w+;$',
    r'^ctx\.pc = ctx\.lr; return;$',
    r'^return;$',
    r'^ctx\.pc = ctx\.ctr; if \(ctx\.pc >= 0x[0-9A-F]+u && ctx\.pc <= 0x[0-9A-F]+u\) goto nwii_entry; nwii_dispatch\(ctx\); return;$',
    r'^ctx\.pc = 0x[0-9A-F]+; if \(nwii::runtime::native_stack_ok\(\)\) [\w:]+\(ctx\); return;$',
)]
# Every other statement with a return/goto must be one of these.
COND_OK = [re.compile(p) for p in (
    r'^if \([^;{}]*\) goto loc_[0-9A-F]+;$',
    r'^if \(ctx\.pc != 0x[0-9A-F]+\) return;$',
    r'^if \([^;{}]*\) \{ ctx\.pc = ctx\.lr; return; \}$',
    r'^if \(process_pending_callbacks\(ctx\)\) return;$',
    r'^if \(ctx\.ctr\) \{ ctx\.pc = ctx\.ctr; nwii_dispatch\(ctx\); if \(ctx\.pc != 0x[0-9A-F]+\) return; \} else nwii::runtime::note_null_call\(0x[0-9A-F]+, ctx\.lr\);$',
    r'^if \([^;{}]*\) \{ nwii::runtime::idle_wait\(ctx\); goto loc_[0-9A-F]+; \}$',
    r'^ctx\.pc = temp_lr; nwii_dispatch\(ctx\); if \(ctx\.pc != 0x[0-9A-F]+\) return;$',
)]


def lname(key):
    if key[0] == 'g': return 'rg%d' % key[1]
    if key[0] == 'f': return 'rf%d' % key[1]
    if key[0] == 'p': return 'rp%d' % key[1]
    if key[0] == 'c': return 'cr%s%d' % (key[2], key[1])
    if key[0] == 'x': return 'xer_l'
    return 'ctr_l'


def cname(key):
    if key[0] == 'g': return 'ctx.gpr[%d]' % key[1]
    if key[0] == 'f': return 'ctx.fpr[%d]' % key[1]
    if key[0] == 'p': return 'ctx.ps1[%d]' % key[1]
    if key[0] == 'c': return 'ctx.cr[%d].%s' % (key[1], key[2])
    if key[0] == 'x': return 'ctx.xer'
    return 'ctx.ctr'


def ctype(key):
    if key[0] in 'fp': return 'double'
    return 'bool' if key[0] == 'c' else 'uint32_t'


def mkey(m):
    if m.group(1) is not None: return ('g', int(m.group(1)))
    if m.group(2) is not None: return ('c', int(m.group(2)), m.group(3))
    if m.lastindex and m.lastindex >= 4 and m.group(4) is not None:
        return ('f' if m.group(4) == 'fpr' else 'p', int(m.group(5)))
    return ('x',) if m.group(0) == 'ctx.xer' else ('t',)


def strip_comment(s):
    i = s.find('//')
    return s[:i].rstrip() if i >= 0 else s.rstrip()


def rewrite_fcmp(line):
    def f(m):
        n, a, b = m.group(1), m.group(2), m.group(3)
        return ('{ const double fa_ = %s, fb_ = %s; const bool un_ = std::isnan(fa_) || std::isnan(fb_); '
                'ctx.cr[%s].lt = !un_ && fa_ < fb_; ctx.cr[%s].gt = !un_ && fa_ > fb_; '
                'ctx.cr[%s].eq = !un_ && fa_ == fb_; ctx.cr[%s].so = un_; }' % (a, b, n, n, n, n))
    line = FCMP.sub(f, line)
    def p(m):
        half = 'fpr' if m.group(1) == '0' else 'ps1'
        return 'ctx.fcmp(%s, ctx.%s[%s], ctx.%s[%s]);' % (m.group(2), half, m.group(3), half, m.group(4))
    if 'ps_cmp' in line:
        line = FCMP.sub(f, PSCMP.sub(p, line))
    return line


# --fpr: FP helpers of CPUContext. INLINE ones are expanded into the same
# expressions as their bodies in cpu_context.h (so their fpr/ps1 accesses become
# plain locals); the others stay calls and are annotated with the FP registers
# they read (R) / write (W): dirty ones among R+W are spilled before the call
# and live ones among W reloaded after it.
NS = 'nwii::runtime::'
def _fill(d, e):
    return '{ const double v_ = %s; ctx.fpr[%s] = v_; ctx.ps1[%s] = v_; }' % (e, d, d)
def _f(r):
    return 'ctx.fpr[%s]' % r
INLINE = {
    'lfs': (2, lambda a: _fill(a[0], '(double)ctx.mmu.read_f32(%s)' % a[1])),
    'stfs': (2, lambda a: 'ctx.mmu.write_f32(%s, %sppc_to_single(%s));' % (a[1], NS, _f(a[0]))),
    'stfiwx': (2, lambda a: 'ctx.mmu.write32(%s, (uint32_t)std::bit_cast<uint64_t>(%s));' % (a[1], _f(a[0]))),
    'fadds': (3, lambda a: _fill(a[0], '%sppc_single(%s + %s)' % (NS, _f(a[1]), _f(a[2])))),
    'fsubs': (3, lambda a: _fill(a[0], '%sppc_single(%s - %s)' % (NS, _f(a[1]), _f(a[2])))),
    'fmuls': (3, lambda a: _fill(a[0], '%sppc_single(%s * %sppc_force25(%s))' % (NS, _f(a[1]), NS, _f(a[2])))),
    'fdivs': (3, lambda a: _fill(a[0], '%sppc_single(%s / %s)' % (NS, _f(a[1]), _f(a[2])))),
    'fres': (2, lambda a: _fill(a[0], '%sppc_fres(%s)' % (NS, _f(a[1])))),
    'frsp': (2, lambda a: _fill(a[0], '%sppc_single(%s)' % (NS, _f(a[1])))),
    'fmadds': (4, lambda a: _fill(a[0], '%sppc_single(%sppc_madd_s(%s, %s, %s))' % (NS, NS, _f(a[1]), _f(a[2]), _f(a[3])))),
    'fmsubs': (4, lambda a: _fill(a[0], '%sppc_single(%sppc_madd_s(%s, %s, -%s))' % (NS, NS, _f(a[1]), _f(a[2]), _f(a[3])))),
    'fnmadds': (4, lambda a: _fill(a[0], '%sppc_neg_keep_nan(%sppc_single(%sppc_madd_s(%s, %s, %s)))' % (NS, NS, NS, _f(a[1]), _f(a[2]), _f(a[3])))),
    'fnmsubs': (4, lambda a: _fill(a[0], '%sppc_neg_keep_nan(%sppc_single(%sppc_madd_s(%s, %s, -%s)))' % (NS, NS, NS, _f(a[1]), _f(a[2]), _f(a[3])))),
    'fmadd': (4, lambda a: '%s = std::fma(%s, %s, %s);' % (_f(a[0]), _f(a[1]), _f(a[2]), _f(a[3]))),
    'fmsub': (4, lambda a: '%s = std::fma(%s, %s, -%s);' % (_f(a[0]), _f(a[1]), _f(a[2]), _f(a[3]))),
    'fnmadd': (4, lambda a: '%s = %sppc_neg_keep_nan(std::fma(%s, %s, %s));' % (_f(a[0]), NS, _f(a[1]), _f(a[2]), _f(a[3]))),
    'fnmsub': (4, lambda a: '%s = %sppc_neg_keep_nan(std::fma(%s, %s, -%s));' % (_f(a[0]), NS, _f(a[1]), _f(a[2]), _f(a[3]))),
    'frsqrte': (2, lambda a: '%s = %sppc_frsqrte(%s);' % (_f(a[0]), NS, _f(a[1]))),
    'fctiwz': (2, lambda a: '%s = std::bit_cast<double>((uint64_t)%sppc_fctiw_image(%sppc_fctiwz(%s), %s));' % (_f(a[0]), NS, NS, _f(a[1]), _f(a[1]))),
    'fctiw': (2, lambda a: '%s = std::bit_cast<double>((uint64_t)%sppc_fctiw_image(%sppc_fctiw_rn(%s, ctx.fpscr), %s));' % (_f(a[0]), NS, NS, _f(a[1]), _f(a[1]))),
}
# Calls kept as calls: (indices of register args read, indices written).
NOFP = {'write_hid2', 'write_dmal', 'dcbz', 'write_dec', 'read_dec', 'mtfsb', 'read_timebase', 'mtfsfi', 'set_fpscr'}
PARTIAL = {'psq_load': ((), (0,)), 'psq_store': ((0,), ()), 'mffs': ((), (0,)), 'mtfsf': ((1,), ())}
CALL = re.compile(r'ctx\.([a-z_0-9]+)\(')


def split_args(s, i):
    """s[i] is just after '('. Returns (args, index after ')')."""
    depth, cur, args = 0, '', []
    while i < len(s):
        ch = s[i]
        if ch in '([': depth += 1
        elif ch in ')]':
            if depth == 0:
                args.append(cur.strip()); return args, i + 1
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(cur.strip()); cur = ''
        else:
            cur += ch
        i += 1
    raise Skip('unbalanced call')


def rewrite_fp(line):
    """Expands INLINE helpers; annotates the others. Returns (line, R, W, all)."""
    R, W, allfp = set(), set(), False
    out, i = '', 0
    code_end = line.find('//')
    while True:
        m = CALL.search(line, i)
        if not m or (code_end >= 0 and m.start() > code_end):
            out += line[i:]; break
        name = m.group(1)
        args, j = split_args(line, m.end())
        regargs = args[:1] if name in ('lfs', 'stfs', 'stfiwx') else args
        if (name in INLINE and line[j:j + 1] == ';' and len(args) == INLINE[name][0]
                and all(re.fullmatch(r'\d+', a) for a in regargs)):
            out += line[i:m.start()] + INLINE[name][1](args)
            i = j + 1
            continue
        if name in PARTIAL or (name.startswith('ps_') and name not in NOFP):
            if name in PARTIAL:
                rd, wr = PARTIAL[name]
            else:
                rd, wr = tuple(range(1, len(args))), (0,)
            for k in rd + wr:
                if not re.fullmatch(r'\d+', args[k]):
                    allfp = True
            if not allfp:
                R |= {int(args[k]) for k in rd}
                W |= {int(args[k]) for k in wr}
        elif name not in NOFP and name in SAFE_HELPERS:
            allfp = True  # an FP helper not expanded (variable register...)
        out += line[i:j]
        i = j
    return out, R, W, allfp


class Skip(Exception):
    pass


def transform_function(lines, stats):
    """lines: the function's lines, first = signature, last = '}'. Returns new lines."""
    body = lines[1:-1]
    if not PRUNE_ONLY and not any(REF.search(l) for l in body) and not (FPR and any(CALL.search(l) for l in body)):
        stats['noref'] += 1
        return lines
    if not PRUNE_ONLY:
        body = [rewrite_fcmp(l) if ('fcmp' in l or 'ps_cmp' in l) else l for l in body]
    fpann = {}
    if FPR:
        nb = []
        for l in body:
            if 'ctx.' in l and '(' in l and CALL.search(l):
                l2, R, W, allfp = rewrite_fp(l)
                if R or W or allfp:
                    fpann[len(nb)] = (R, W, allfp)
                nb.append(l2)
            else:
                nb.append(l)
        body = nb
    # Locate the trace line, the entry switch and the label nwii_entry.
    i = 0
    head = []
    if body and 'g_trace_calls' in body[0]:
        head.append(body[0]); i = 1
    nodes = []  # dict(kind, lines, ...)
    n = len(body)
    while i < n:
        l = body[i]
        s = l.strip()
        if s.startswith('if (ctx.pc != 0x') and s.endswith('{') and i + 1 < n and body[i + 1].strip() == 'switch (ctx.pc) {':
            j = i + 1
            while body[j] != '    }':
                j += 1
            sw = body[i:j + 1]
            if ENTRIES is not None and not any(LABEL.match(b.strip()) and b.strip().startswith('nwii_entry') for b in body):
                kept = []
                for b in sw:
                    cm = CASE.match(b)
                    if cm and int(cm.group(1), 16) not in ENTRIES:
                        stats['pruned'] += 1
                        continue
                    if cm:
                        stats['kept'] += 1
                    kept.append(b)
                if len(kept) != len(sw):
                    kept = [b.replace('default: nwii::runtime::interpret_step(ctx); return;',
                                      'default: nwii::runtime::resume_pruned(ctx); return;') for b in kept]
                    stats['pruned_fn'] += 1
                sw = kept
            if PRUNE_ONLY:
                stats['done'] += 1
                return [lines[0]] + body[:i] + sw + body[j + 1:] + [lines[-1]]
            nodes.append({'kind': 'switch', 'lines': sw})
            i = j + 1
            continue
        if l == '    {':
            # instruction-level block: raw if it indexes registers with a variable
            j = i + 1
            while not body[j].startswith('    }'):
                j += 1
            blk = body[i:j + 1]
            if any(re.search(r'ctx\.(gpr|cr)\[[a-z_]', b) or 'for (' in b for b in blk):
                for b in blk:
                    if 'goto' in b or 'return' in b:
                        raise Skip('raw block with control flow')
                nodes.append({'kind': 'raw', 'lines': blk})
                i = j + 1
                continue
            # otherwise: lines handled one by one
        nodes.append({'kind': 'line', 'lines': [l], 'fp': fpann.get(i)})
        i += 1
    if PRUNE_ONLY:  # function without an entry switch: unchanged
        stats['noswitch'] = stats.get('noswitch', 0) + 1
        return lines
    # Classify line nodes.
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
            nd['kind'] = 'label'; nd['name'] = lm.group(1); label_idx[lm.group(1)] = k
            continue
        esc = bool(ESCAPE.search(code))
        for m in CTX_MEMBER.finditer(code):
            name = m.group(1)
            if name in SAFE_FIELDS: continue
            if name in SAFE_HELPERS and code[m.end():m.end() + 1] == '(': continue
            esc = True
        has_flow = ('return' in code) or ('goto' in code)
        uncond = any(p.match(s) for p in UNCOND)
        if has_flow and not uncond and not any(p.match(s) for p in COND_OK):
            raise Skip('unknown flow: ' + s)
        nd['uncond'] = uncond
        nd['gotos'] = GOTO.findall(code)
        for m in REF.finditer(code):
            keys.setdefault(mkey(m), None)
        # inside a nested block (if-body, else, sraw's two arms...): the
        # statement may not run, so it must not kill liveness or reset dirt.
        nd['cond'] = (len(l) - len(l.lstrip())) > 4
        if esc:
            nd['kind'] = 'esc'
            # reload before a goto only if the escape call runs before it
            gm = re.search(r'goto (loc_[0-9A-F]+|nwii_entry);', code)
            em = ESCAPE.search(code)
            nd['reload_before_goto'] = bool(gm and em and em.start() < gm.start())
            nd['goto_bypasses'] = bool(gm and em and gm.start() < em.start())
        else:
            nd['kind'] = 'pure'
            nd['ret'] = 'return' in code
    keylist = sorted(keys, key=lambda k: (k[0], k[1:] if len(k) > 1 else ()))
    bit = {k: 1 << b for b, k in enumerate(keylist)}
    ALL = (1 << len(keylist)) - 1
    # defs/uses of pure lines
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
    # FP helper calls kept as calls (--fpr): spill R+W if dirty, reload W if live.
    fpmask = 0
    for kk in keylist:
        if kk[0] in 'fp': fpmask |= bit[kk]
    for nd in nodes:
        nd['hrw'] = nd['hw'] = 0
        if nd['kind'] == 'pure' and nd.get('fp'):
            R, W, allfp = nd['fp']
            if allfp:
                rw = w = fpmask
            else:
                rw = w = 0
                for r in R | W:
                    for t in 'fp':
                        rw |= bit.get((t, r), 0)
                for r in W:
                    for t in 'fp':
                        w |= bit.get((t, r), 0)
            nd['hrw'] = rw; nd['hw'] = w
            if (rw or w) and nd['ret']:
                raise Skip('fp helper on a return line')
    # CFG successors
    N = len(nodes)
    labels = [label_idx[x] for x in label_idx if x != 'nwii_entry']
    succ = [[] for _ in range(N)]
    for k, nd in enumerate(nodes):
        kd = nd['kind']
        if kd in ('line',):
            raise Skip('internal')
        if kd in ('pure', 'esc'):
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
    # Forward may-dirty analysis.
    din = [0] * N
    dout = [0] * N
    work = list(range(N))
    inq = [True] * N
    while work:
        k = work.pop()
        inq[k] = False
        x = 0
        for p in pred[k]:
            x |= dout[p]
        din[k] = x
        nd = nodes[k]
        kd = nd['kind']
        if kd == 'pure':
            o = (x if nd['cond'] else (x & ~nd['hrw'])) | nd['def']
        elif kd == 'esc':
            o = x if nd['cond'] else 0
        elif kd == 'raw':
            o = 0
        else:
            o = x
        if o != dout[k]:
            dout[k] = o
            for t in succ[k]:
                if not inq[t]:
                    inq[t] = True; work.append(t)
    # Backward liveness. Escapes/raw/returns read the dirty set (spill).
    lin = [0] * N
    lout = [0] * N
    work = list(range(N))
    inq = [True] * N
    while work:
        k = work.pop()
        inq[k] = False
        x = 0
        for t in succ[k]:
            x |= lin[t]
        lout[k] = x
        nd = nodes[k]
        kd = nd['kind']
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
        else:
            i_ = x
        if i_ != lin[k]:
            lin[k] = i_
            for p in pred[k]:
                if not inq[p]:
                    inq[p] = True; work.append(p)

    def spill(mask):
        return ''.join('%s = %s; ' % (cname(kk), lname(kk)) for kk in keylist if mask & bit[kk])

    def reload(mask):
        return ''.join('%s = %s; ' % (lname(kk), cname(kk)) for kk in keylist if mask & bit[kk])

    def rename(code):
        return REF.sub(lambda m: lname(mkey(m)), code)

    out = [lines[0]]
    out.extend(head)
    decl = '    ' + ' '.join('%s %s = %s;' % (ctype(kk), lname(kk), cname(kk)) for kk in keylist)
    out.append(decl)
    for k, nd in enumerate(nodes):
        kd = nd['kind']
        if kd in ('switch', 'label'):
            out.extend(nd['lines']); continue
        if kd == 'raw':
            sp = spill(din[k])
            if sp: out.append('    ' + sp.rstrip())
            out.extend(nd['lines'])
            rl = reload(lout[k])
            if rl: out.append('    ' + rl.rstrip())
            stats['raw'] += 1
            continue
        l = nd['lines'][0]
        ind = l[:len(l) - len(l.lstrip())]
        body_ = l.lstrip()
        if kd == 'esc':
            stats['esc'] += 1
            sp = spill(din[k])
            rl = reload(lout[k])
            if nd['reload_before_goto'] and rl:
                body_ = re.sub(r'goto (loc_[0-9A-F]+);', lambda m: rl + m.group(0), body_, count=1)
            ci = body_.find('//')
            c_, cm_ = (body_[:ci].rstrip(), ' ' + body_[ci:]) if ci >= 0 else (body_, '')
            line = ind + sp + c_
            if not nd['uncond'] and rl:
                line += ' ' + rl.rstrip()
            out.append(line + cm_)
            continue
        # pure: rename the statement first, then add the spill (which must
        # keep its ctx.* = local text).
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
                raise Skip('pure return form: ' + code)
            stats['retspill'] += 1
        if nd['hrw'] or nd['hw']:
            ci = code.find('//')
            c_, cm_ = (code[:ci].rstrip(), ' ' + code[ci:]) if ci >= 0 else (code, '')
            sp = spill(nd['hrw'] & din[k])
            rl = reload(nd['hw'] & lout[k])
            code = sp + c_ + ((' ' + rl.rstrip()) if rl else '') + cm_
            stats['fpcall'] += 1
        out.append(ind + code)
    out.append(lines[-1])
    stats['done'] += 1
    return out


ENTRY_REF = re.compile(r'(?:ctx\.(?:pc|lr|srr0) = |ctx\.pc [!=]= )0x([0-9A-F]+)')
RUN_GAME_CASE = re.compile(r'^            case 0x([0-9A-F]+): \w+\(ctx\); break;$', re.M)


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


LFS_STFS = re.compile(r'ctx\.(lfs|stfs)\(')


def memview(fn, stats):
    """fn: function lines. ctx.mmu.X( -> mv_.X(, lfs/stfs expanded on mv_."""
    body = fn[1:-1]
    if not any('ctx.mmu.' in l or LFS_STFS.search(l) for l in body):
        return fn
    out = []
    for l in body:
        if LFS_STFS.search(l):
            res, i = '', 0
            while True:
                m = LFS_STFS.search(l, i)
                if not m:
                    res += l[i:]; break
                args, j = split_args(l, m.end())
                if len(args) != 2 or l[j:j + 1] != ';':
                    raise Skip('lfs/stfs form')
                if m.group(1) == 'lfs':
                    res += l[i:m.start()] + 'ctx.fill(%s, (double)mv_.read_f32(%s))' % (args[0], args[1])
                else:
                    res += l[i:m.start()] + 'mv_.write_f32(%s, nwii::runtime::ppc_to_single(ctx.fpr[%s]))' % (args[1], args[0])
                i = j
                stats['lfsstfs'] = stats.get('lfsstfs', 0) + 1
            l = res
        if 'ctx.mmu.' in l:
            stats['mmu'] = stats.get('mmu', 0) + l.count('ctx.mmu.')
            l = l.replace('ctx.mmu.', 'mv_.')
        out.append(l)
    stats['memview_fn'] = stats.get('memview_fn', 0) + 1
    return [fn[0], '    nwii::runtime::MemView mv_(ctx.mmu);'] + out + [fn[-1]]


def process_file(path, stats):
    with open(path) as f:
        src = f.read().split('\n')
    if src and src[0] == MARK:
        sys.exit('already applied: ' + path)
    res = [MARK]
    i = 0
    n = len(src)
    while i < n:
        l = src[i]
        if l.startswith('void ') and l.endswith('(CPUContext& ctx) {'):
            j = i + 1
            while src[j] != '}':
                j += 1
            fn = src[i:j + 1]
            try:
                fn2 = transform_function(fn, stats)
                if MEMVIEW:
                    fn2 = memview(fn2, stats)
                res.extend(fn2)
            except Skip as e:
                stats['skip'] += 1
                stats.setdefault('why', {}).setdefault(str(e)[:60], 0)
                stats['why'][str(e)[:60]] += 1
                res.extend(fn)
            i = j + 1
            continue
        res.append(l)
        i += 1
    tmp = path + '.tmp'
    with open(tmp, 'w') as f:
        f.write('\n'.join(res))
    os.replace(tmp, path)


def main():
    d = sys.argv[1]
    files = [a for a in sys.argv[2:] if not a.startswith('--')] or sorted(os.path.join(d, x) for x in os.listdir(d)
                                   if re.match(r'output_\d+\.cpp$', x))
    global ENTRIES
    global FPR, REF, PRUNE_ONLY, MEMVIEW
    PRUNE_ONLY = '--prune-only' in sys.argv
    MEMVIEW = '--memview' in sys.argv
    if '--fpr' in sys.argv:
        FPR = True
        REF = re.compile(REF_BASE + r'|ctx\.(fpr|ps1)\[(\d+)\]')
    stats = {'done': 0, 'skip': 0, 'noref': 0, 'esc': 0, 'raw': 0, 'retspill': 0, 'fpcall': 0,
             'kept': 0, 'pruned': 0, 'pruned_fn': 0}
    if '--no-prune' not in sys.argv:
        ENTRIES = collect_entries(d)
        print('entries:', len(ENTRIES))
    files = [f for f in files if not f.startswith('--')]
    for p in files:
        process_file(p, stats)
    why = stats.pop('why', {})
    print('reglocals:', stats)
    for k, v in sorted(why.items(), key=lambda x: -x[1])[:20]:
        print('  skip %5d  %s' % (v, k))


if __name__ == '__main__':
    main()
