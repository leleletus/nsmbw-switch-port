#!/usr/bin/env python3
"""apply_n1.py <export_dir> [--orig <r18m export dir>] [--list]      (r9/r19n, front N)

Native 1:1 bodies for hot SDK leaf functions of the game thread. For every function of TABLE:

  kind 'fp'  (paired-single / float leaf functions: PSMTX*, nw4r::math SinFIdx/CosFIdx...): a copy of the
             generated body ("native variant" n1v_<addr>) with the guest FPRs in C++ locals: ctx.fpr[N] /
             ctx.ps1[N] -> fN / pN, the CPUContext FP helpers -> the N1_* macros of native_n1_fp.h (the same
             code on the locals), psq_l_fast / psq_st_fast -> N1_PSQ_L / N1_PSQ_ST (same MemView accesses;
             the quantized GQR types inline instead of the out-of-line CPUContext::psq_load/psq_store).
             Everything else stays character for character (G5 locals, memory accesses, inst_count, CR,
             branches, interrupt polls). The FP locals go back to ctx at every exit and around the slow path
             of every poll (and are reloaded after it). Only at the entry (ctx.pc == entry): the entry
             switch is dropped. Refused if the function calls anything with ctx (not a leaf) or uses a ctx
             member the translation does not know.
  kind 'int'  the same for integer leaf functions with loops (strcmp, memcpy...): only the polls change.
  Every kind: the interrupt polls of the body are batched (N1Poll, native_n1.h): a poll that the inline part
             of the generated poll would find quiet only counts down a local copy of poll_left; the others
             are done for real, with the same spills. Same outcome as the generated code, minus the per-poll
             ctx.pc store and atomic reads.
  kind 'n2'   round N2: hand-written single-precision body in the runtime (native_n2.cpp, n2h_<addr>), exact
             fallback to the generated code; only the hook is added.
  kind 'hand' hand-written body in the runtime (native_n1.cpp, n1h_<addr>); only the hook is added.

The generated function gets one line at the top:
    if (ctx.pc == 0xADDR && nwii::runtime::n1_hook(n1s_ADDR, ctx)) return;  // [n1]
and stays as it was below it (fallback, reference for the verify mode). n1_hook / the switches:
nWiiRuntime/include/runtime/native_n1.h. Each touched file gets #include "runtime/native_n1_fp.h" and the
mark // [n1]; the pass refuses to run twice and checks each function's sha256 against r18m's export.
"""
import argparse, hashlib, os, re, sys

# (address, generated name, short name for NWII_NATIVE_<name>, kind)
TABLE = [
    (0x801C04D0, 'PSMTXIdentity__mtx_a_mtx_o_801C04D0', 'PSMTXIDENTITY', 'n2'),
    (0x801C0500, 'PSMTXCopy__mtx_a_mtx_o_801C0500', 'PSMTXCOPY', 'n2'),
    (0x801C0540, 'PSMTXConcat_801C0540', 'PSMTXCONCAT', 'n2'),
    (0x801C0610, 'func_801C0610', 'PSMTXCONCATARRAY', 'fp'),
    (0x801C07A0, 'func_801C07A0', 'PSMTXINVERSE', 'n2'),
    (0x801C08A0, 'func_801C08A0', 'PSMTXINVXPOSE', 'fp'),
    (0x80237BD0, 'func_80237BD0', 'SINFIDX', 'n2'),
    (0x80237C40, 'func_80237C40', 'COSFIDX', 'n2'),
    (0x80237CA0, 'func_80237CA0', 'SINCOSFIDX', 'fp'),
    (0x802382B0, 'func_802382B0', 'MATH802382B0', 'n2'),
    (0x801C0BD0, 'PSMTXTrans_801C0BD0', 'PSMTXTRANS', 'n2'),
    (0x801C1160, 'PSMTXMultVec__mtx_a_mtxvec_o_801C1160', 'PSMTXMULTVEC', 'n2'),
    (0x801C6FF0, 'GXInitTexObjLOD__gx_a_GXTexture_o_801C6FF0', 'GXINITTEXOBJLOD', 'fp'),
    (0x801C9940, 'func_801C9940', 'GXLOADPOSMTXIMM', 'n2'),
    (0x801C9A80, 'func_801C9A80', 'GXLOADTEXMTXIMM', 'n2'),
    (0x802381A0, 'func_802381A0', 'MATH802381A0', 'fp'),
    (0x802390D0, 'func_802390D0', 'MATH802390D0', 'fp'),
    (0x80240100, 'func_80240100', 'ANM80240100', 'fp'),
    (0x80240240, 'func_80240240', 'ANM80240240', 'fp'),
    (0x802403C0, 'func_802403C0', 'ANM802403C0', 'n2'),
    (0x802DCC4C, '__cvt_fp2unsigned__Runtime_PPCEABI_H_a_runtime_o_802DCC4C', 'CVTFP2UNSIGNED', 'n2'),
    (0x8000D0D0, 'func_8000D0D0', 'GAME8000D0D0', 'fp'),
    (0x802835B0, 'func_802835B0', 'SND802835B0', 'fp'),
    (0x80283760, 'func_80283760', 'SND80283760', 'fp'),
    (0x802837C0, 'func_802837C0', 'SND802837C0', 'fp'),
    (0x802838A0, 'func_802838A0', 'SND802838A0', 'fp'),
    (0x80004364, 'func_80004364', 'MEMCPY', 'fp'),
    (0x80004600, '__fill_mem_80004600', 'FILLMEM', 'int'),
    (0x802E1AB4, 'strcmp__MSL_C_PPCEABI_bare_H_a_string_o_802E1AB4', 'STRCMP', 'n2'),
    (0x801D8190, 'func_801D8190', 'CXUNCOMPRESSLZ', 'int'),
    (0x80162C30, 'func_80162C30', 'SEARCHNODEBYID', 'hand'),
    (0x801AC440, 'DCInvalidateRange_801AC440', 'DCINVALIDATERANGE', 'hand'),
    (0x801AC470, 'DCFlushRange_801AC470', 'DCFLUSHRANGE', 'hand'),
]

MARK = '// [n1]'
FP_OPS = {  # macro name -> indices of the register arguments
    'fill': [0], 'set_bits': [0], 'fadds': [0, 1, 2], 'fsubs': [0, 1, 2], 'fmuls': [0, 1, 2], 'fdivs': [0, 1, 2],
    'fres': [0, 1], 'fmadds': [0, 1, 2, 3], 'fmsubs': [0, 1, 2, 3], 'fnmadds': [0, 1, 2, 3], 'fnmsubs': [0, 1, 2, 3],
    'frsp': [0, 1], 'fmadd': [0, 1, 2, 3], 'fmsub': [0, 1, 2, 3], 'fnmadd': [0, 1, 2, 3], 'fnmsub': [0, 1, 2, 3],
    'frsqrte': [0, 1], 'fctiw': [0, 1], 'fctiwz': [0, 1],
    'ps_add': [0, 1, 2], 'ps_sub': [0, 1, 2], 'ps_mul': [0, 1, 2], 'ps_div': [0, 1, 2],
    'ps_madd': [0, 1, 2, 3], 'ps_msub': [0, 1, 2, 3], 'ps_nmadd': [0, 1, 2, 3], 'ps_nmsub': [0, 1, 2, 3],
    'ps_sum0': [0, 1, 2, 3], 'ps_sum1': [0, 1, 2, 3], 'ps_muls0': [0, 1, 2], 'ps_muls1': [0, 1, 2],
    'ps_madds0': [0, 1, 2, 3], 'ps_madds1': [0, 1, 2, 3], 'ps_res': [0, 1], 'ps_rsqrte': [0, 1],
    'ps_sel': [0, 1, 2, 3], 'ps_mr': [0, 1], 'ps_neg': [0, 1], 'ps_abs': [0, 1], 'ps_nabs': [0, 1],
    'ps_merge00': [0, 1, 2], 'ps_merge01': [0, 1, 2], 'ps_merge10': [0, 1, 2], 'ps_merge11': [0, 1, 2],
}
# ctx members the translated body may still use (G5 spills, CR, memory view, GQR/FPSCR in macros)
CTX_OK = re.compile(r'ctx\.(gpr\[\d+\]|cr\[\d\]\.(lt|gt|eq|so)|inst_count|pc|lr|ctr|xer|mmu)(?![A-Za-z0-9_])|ctx\.fcmp\(|nwii::runtime::poll_due\(ctx\)|nwii::runtime::process_pending_callbacks_slow\(ctx\)')


def split_args(s):
    out, depth, cur = [], 0, ''
    for ch in s:
        if ch == ',' and depth == 0:
            out.append(cur.strip()); cur = ''
            continue
        if ch in '([{': depth += 1
        if ch in ')]}': depth -= 1
        cur += ch
    out.append(cur.strip())
    return out


def find_function(text, name):
    m = re.search(r'^void %s\(CPUContext& ctx\) \{\n' % re.escape(name), text, re.M)
    if not m:
        return None
    return m.start(), text.index('\n}\n', m.end()) + 3


def translate(fn_text, name, addr):
    m = re.search(r'g_trace_calls && ctx\.pc == (0x[0-9A-F]{8})\)', fn_text)
    if not m or int(m.group(1), 16) != addr:
        sys.exit('apply_n1: %s: entry %s is not the function address (merged function): not handled' % (name, m and m.group(1)))
    lines = fn_text.split('\n')
    assert lines[0].startswith('void %s(' % name)
    body = lines[1:]
    while body and body[-1] == '':
        body.pop()
    assert body[-1] == '}'
    body = body[:-1]
    regs = set()
    npolls = [0]
    out = []
    i = 0
    # drop the entry switch: "    if (ctx.pc != 0xADDR) {" ... "    }"
    entry_if = '    if (ctx.pc != 0x%08X) {' % addr
    while i < len(body):
        ln = body[i]
        if ln == entry_if:
            j = i + 1
            while body[j] != '    }':
                j += 1
            i = j + 1
            continue
        if 'g_trace_calls' in ln:
            i += 1
            continue
        if 'goto nwii_entry' in ln:
            sys.exit('apply_n1: %s uses nwii_entry (self tail call): not handled' % name)
        out.append(ln)
        i += 1
    # interrupt polls: "ctx.pc = 0xX;" + the poll right after it (comments in between) -> one batched poll
    poll_pc = {}
    for i, ln in enumerate(out):
        m = re.match(r'^\s*ctx\.pc = (0x[0-9A-F]{8});$', ln)
        if not m:
            continue
        j = i + 1
        while j < len(out) and out[j].strip().startswith('//'):
            j += 1
        if j < len(out) and ('nwii::runtime::poll_due(ctx)' in out[j] or out[j].strip() == 'if (process_pending_callbacks(ctx)) return;'):
            poll_pc[j] = m.group(1)
            out[i] = None
    if any('poll_due' in (l or '') or 'process_pending_callbacks' in (l or '') for k, l in enumerate(out) if k not in poll_pc):
        sys.exit('apply_n1: %s: a poll without its "ctx.pc = " line' % name)
    res = []
    for k, ln in enumerate(out):
        if ln is None:
            continue
        code, sep, comment = ln.partition(' //')
        if code.strip().startswith('//'):
            res.append(ln)
            continue
        # interrupt poll of a G5 function: FP locals to ctx around the slow path
        # a poll: quiet ones only count down n1p_ (N1Poll, native_n1.h); the others as in the generated code
        m = re.match(r'^(\s*)if \(NWII_UNLIKELY\(nwii::runtime::poll_due\(ctx\)\)\) \{ (.*)if \(nwii::runtime::process_pending_callbacks_slow\(ctx\)\) return; (.*)\}$', code)
        m2 = re.match(r'^(\s*)if \(process_pending_callbacks\(ctx\)\) return;$', code)
        if m or m2:
            ind, spill, reload = m.groups() if m else (m2.group(1), '', '')
            res.append('%sif (NWII_UNLIKELY(!n1p_.fast())) { n1p_.flush(); ctx.pc = %s; if (nwii::runtime::poll_due(ctx)) { %s@FPSPILL@ ++nwii::runtime::g_n1_slow; '
                       'if (nwii::runtime::process_pending_callbacks_slow(ctx)) return true; @FPLOAD@ %s} n1p_.reload(); }  // [n1] poll'
                       % (ind, poll_pc[k], spill, reload))
            npolls[0] += 1
            continue
        # psq (anywhere in the statement: G5 may put "ctx.inst_count = G5ic; " before an MMIO one)
        def psq_fast(mm):
            kind, sid, r, ea, w, q = mm.groups()
            regs.add(int(r))
            return 'N1_PSQ_%s(%s, %s, %s, %s, %s);' % ('L' if kind == 'l' else 'ST', sid, r, ea, w, q)
        def psq_io(mm):
            r, ea, w, q = mm.groups()
            regs.add(int(r))
            return 'N1_PSQ_ST_IO(%s, %s, %s, %s);' % (r, ea, w, q)
        code = re.sub(r'nwii::runtime::psq_(l|st)_fast<(\d+u)>\(ctx, mv_, (\d+), ([^;]*), (\d+), (\d+)\);', psq_fast, code)
        code = re.sub(r'nwii::runtime::psq_st_io\(ctx, mv_, (\d+), ([^;]*), (\d+), (\d+)\);', psq_io, code)
        if 'psq_' in code and ('psq_l_io' in code or 'psq_load' in code or 'psq_store' in code):
            sys.exit('apply_n1: %s: psq form not handled: %s' % (name, ln))
        # FP helper calls
        def op(mm):
            nm, args = mm.group(1), split_args(mm.group(2))
            if nm not in FP_OPS:
                sys.exit('apply_n1: %s: ctx.%s not handled: %s' % (name, nm, ln))
            for k in FP_OPS[nm]:
                if not re.fullmatch(r'\d+', args[k]):
                    sys.exit('apply_n1: %s: non-constant register in %s' % (name, ln))
                regs.add(int(args[k]))
            return 'N1_%s(%s)' % (nm, ', '.join(args))
        code2 = code
        pos = 0
        while True:
            mm = re.compile(r'ctx\.([a-z_0-9]+)\(').search(code2, pos)
            if not mm:
                break
            depth, k = 1, mm.end()
            while depth:
                depth += {'(': 1, ')': -1}.get(code2[k], 0)
                k += 1
            nm, args = mm.group(1), split_args(code2[mm.end():k - 1])
            if nm == 'fcmp':  # CPUContext::fcmp(crD, double a, double b): kept, its operands become locals below
                pos = mm.end()
                continue
            if nm not in FP_OPS:
                sys.exit('apply_n1: %s: ctx.%s not handled: %s' % (name, nm, ln))
            for q in FP_OPS[nm]:
                if not re.fullmatch(r'\d+', args[q]):
                    sys.exit('apply_n1: %s: non-constant register in %s' % (name, ln))
                regs.add(int(args[q]))
            rep = 'N1_%s(%s)' % (nm, ', '.join(args))
            code2 = code2[:mm.start()] + rep + code2[k:]
            pos = mm.start() + len(rep)
        for mm in re.finditer(r'ctx\.(fpr|ps1)\[(\d+)\]', code2):
            regs.add(int(mm.group(2)))
        code2 = re.sub(r'ctx\.fpr\[(\d+)\]', r'f\1', code2)
        code2 = re.sub(r'ctx\.ps1\[(\d+)\]', r'p\1', code2)
        # exits
        code2 = code2.replace('ctx.pc = ctx.lr; return;', '@FLUSH@@FPSPILL@ ctx.pc = ctx.lr; return true;')
        if re.search(r'\breturn;', code2):
            sys.exit('apply_n1: %s: return form not handled: %s' % (name, ln))
        # anything else that reaches ctx
        rest = CTX_OK.sub('', code2).replace('mv_(ctx.mmu)', '')
        if re.search(r'\bctx\b', rest):
            sys.exit('apply_n1: %s: ctx use not handled: %s' % (name, ln))
        if re.search(r'\bfunc_[0-9A-F]{8}\(|_[0-9A-F]{8}\(ctx\)', code2):
            sys.exit('apply_n1: %s calls another guest function (not a leaf): %s' % (name, ln))
        res.append(code2 + ((sep + comment) if sep else ''))
    rl = sorted(regs)
    spill = ' '.join('ctx.fpr[%d] = f%d; ctx.ps1[%d] = p%d;' % (r, r, r, r) for r in rl)
    load = ' '.join('f%d = ctx.fpr[%d]; p%d = ctx.ps1[%d];' % (r, r, r, r) for r in rl)
    decl = '    double ' + ', '.join('f%d = ctx.fpr[%d], p%d = ctx.ps1[%d]' % (r, r, r, r) for r in rl) + ';  // [n1] FPRs in locals' if rl else ''
    if npolls[0]:
        decl = (decl + '\n' if decl else '') + '    nwii::runtime::N1Poll n1p_(ctx);  // [n1] %d interrupt polls batched' % npolls[0]
    final = []
    for ln in res:
        ln = ln.replace('@FPSPILL@', spill).replace('@FPLOAD@', load).replace('@FLUSH@', 'n1p_.flush(); ' if npolls[0] else '')
        final.append(ln)
        if ln.rstrip().endswith('// [r14-g5]') and decl:
            final.append(decl)
            decl = ''
    if decl:  # no G5 locals line: after the MemView line (or first)
        k = next((i for i, l in enumerate(final) if 'MemView mv_(ctx.mmu)' in l), -1)
        final.insert(k + 1, decl)
    head = ['%s native variant of %s (r9/r19n/tools/apply_n1.py): FPRs in locals, entry only' % (MARK, name),
            'static NWII_NOINLINE bool n1v_%08X(CPUContext& ctx) {' % addr]
    return '\n'.join(head + final + ['}', '']), rl


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('export')
    ap.add_argument('--orig', default='/home/djvemo/nsmbw-port/r9/r18m/export',
                    help='unmodified export the files must equal (r18m); "" = no check (a regenerated export, e.g. G6)')
    ap.add_argument('--only', default='')
    ap.add_argument('--skip-untranslatable', action='store_true',
                    help='regenerated export: a function the fp/int translation refuses gets no hook (warning) instead of an error')
    a = ap.parse_args()
    files = {}
    # every generated function by address (the name ends in _<ADDR>: func_<ADDR> or <symbol>_<ADDR>); one read per file
    where = {}
    for f in sorted(os.listdir(a.export)):
        if f.startswith('output_') and f.endswith('.cpp'):
            for m in re.finditer(r'^void (\w*_([0-9A-F]{8}))\(CPUContext& ctx\) \{', open(os.path.join(a.export, f)).read(), re.M):
                where.setdefault(int(m.group(2), 16), []).append((f, m.group(1)))
    for addr, name, short, kind in TABLE:
        if a.only and short not in a.only.split(','):
            continue
        hits = where.get(addr, [])
        if len(hits) != 1:
            sys.exit('apply_n1: 0x%08X (%s) found in %s' % (addr, name, hits))
        files.setdefault(hits[0][0], []).append((addr, hits[0][1], short, kind))
    for f, fns in sorted(files.items()):
        p = os.path.join(a.export, f)
        text = open(p).read()
        if MARK in text:
            sys.exit('apply_n1: %s already has %s' % (f, MARK))
        if a.orig and text != open(os.path.join(a.orig, f)).read():
            sys.exit('apply_n1: %s differs from %s' % (p, a.orig))
        done = []
        for addr, name, short, kind in fns:
            a0, b0 = find_function(text, name)
            fn = text[a0:b0]
            sha = hashlib.sha256(fn.encode()).hexdigest()
            pre = ''
            post = ''
            if kind in ('fp', 'int'):
                try:
                    native, rl = translate(fn, name, addr)
                except SystemExit as e:
                    if not a.skip_untranslatable:
                        raise
                    print('apply_n1: WARNING %s left without hook: %s' % (name, e))
                    continue
                pre = 'static bool n1v_%08X(CPUContext& ctx);  %s\n' % (addr, MARK)
                post = '\n' + native
                natfn = 'n1v_%08X' % addr
                info = 'fp, %d FPRs in locals' % len(rl)
            elif kind == 'n2':
                natfn = 'nwii::runtime::n2h_%08X' % addr
                info = 'hand-written N2 (native_n2.cpp)'
                # declared here (not in native_n1.h): adding a body recompiles only the file of its function
                pre = 'namespace nwii { namespace runtime { bool n2h_%08X(CPUContext& ctx); } }  %s\n' % (addr, MARK)
            else:
                natfn = 'nwii::runtime::n1h_%08X' % addr
                info = 'hand-written (native_n1.cpp)'
            flags = 1 if re.search(r'_io\(', fn) else 0  # MMIO / gather pipe writes: never in the verify mode
            pre += 'static nwii::runtime::N1Slot n1s_%08X = {0x%08Xu, "%s", &%s, &%s, %du};  %s\n' % (addr, addr, short, natfn, name, flags, MARK)
            first = fn.index('\n') + 1
            # direct call of the native body in the usual case (mode 1); the rest through n1_hook_slow
            hook = ('    if (ctx.pc == 0x%08X && (NWII_LIKELY(n1s_%08X.mode == 1) ? %s(ctx) : nwii::runtime::n1_hook_slow(n1s_%08X, ctx))) return;  %s\n'
                    % (addr, addr, natfn, addr, MARK))
            new = pre + fn[:first] + hook + fn[first:] + post
            text = text[:a0] + new + text[b0:]
            done.append(name)
            print('apply_n1: %s %s (0x%08X) %s, sha256 %s' % (f, name, addr, info, sha[:16]))
        text = text.replace('#include "functions.h"\n', '#include "functions.h"\n#include "runtime/native_n1_fp.h"  %s\n#include "runtime/native_n1.h"  %s\n' % (MARK, MARK), 1)
        text = '%s native bodies of hot SDK leaf functions (r9/r19n/tools/apply_n1.py): %s\n' % (MARK, ', '.join(done)) + text
        open(p, 'w').write(text)


if __name__ == '__main__':
    main()
