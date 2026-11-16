#!/usr/bin/env python3
"""R11c: recompile-time classification of guest memory-access sites.

static_mmio.py <export_dir> <sites.tsv> <out.tsv>

Rebuilds every generated function's PowerPC instruction stream from the
`// 0xADDR: OPCODE` comments and runs a forward constant propagation of the
GPRs over the function's control-flow graph (lis/addis/addi/li/ori/oris/
xori/andi./mr/rlwinm with known inputs; every other GPR write -> unknown;
calls clobber r0 and r3-r12; jump-table and resume entries start unknown).
A memory access whose effective address is a known constant is classified by
region (MMIO block, gather pipe, locked cache, RAM, ...); otherwise 'unknown'.

Only a performance hint: a wrongly classified site stays correct (an MMIO site
without the explicit slow call takes the fault path; a RAM site with it just
keeps the old check).

out.tsv: id pc kind static_class ea
"""
import collections
import re
import sys

RX_FUNC = re.compile(r'^(?:static\s+)?(?:NWII_NOINLINE\s+)?void\s+(\w+)\(CPUContext\s*&\s*ctx\)\s*\{')
RX_PC = re.compile(r'^\s*// 0x([0-9A-Fa-f]{8}): ([0-9A-Fa-f]{8})\s*$')
M32 = 0xFFFFFFFF


def sext16(x):
    return x - 0x10000 if x & 0x8000 else x


def region(ea):
    p = ea & 0x3FFFFFFF
    if (ea - 0xE0000000) & M32 < 0x40000:
        return 'LC'
    if p < 0x10:
        return 'low16'
    if p < 0x1000:
        return 'page0'
    if p < 0x01800000:
        return 'RAM'
    if 0x10000000 <= p < 0x14000000:
        return 'RAM'
    if p < 0x0C000000:
        return 'unmapped'
    if p < 0x0C010000:
        return {0: 'CP', 1: 'PE', 2: 'VI', 3: 'PI', 4: 'MI', 5: 'DSP', 6: 'DI/SI/EXI/AI',
                8: 'WGPIPE'}.get((p >> 12) & 0xF, 'HW0C_other')
    if 0x0D000000 <= p < 0x0D010000:
        return 'HW0D'
    if 0x0D800000 <= p < 0x0D900000:
        return 'HW0D8'
    return 'unmapped'


def rotl(x, n):
    n &= 31
    return ((x << n) | (x >> (32 - n))) & M32 if n else x


def mask(mb, me):
    m = 0
    i = mb
    while True:
        m |= 1 << (31 - i)
        if i == me:
            break
        i = (i + 1) & 31
    return m


ST_NOWRITE_31 = {0, 32, 4, 144, 146, 210, 467, 242, 151, 215, 407, 662, 918, 150, 725, 663, 727, 983, 1014,
                 86, 54, 470, 278, 246, 982, 598, 854, 661, 149}
UPD_31 = {55, 119, 311, 375, 183, 247, 439, 567, 631, 695, 759}
DEST_RA_31 = {28, 60, 124, 284, 316, 412, 444, 476, 24, 536, 792, 824, 26, 922, 954}
LOAD_D = {32, 33, 34, 35, 40, 41, 42, 43}
UPD_D = {33, 35, 37, 39, 41, 43, 45, 49, 51, 53, 55, 57, 61}
MEM_D = set(range(32, 56)) | {56, 57, 60, 61}
MEM_X_31 = {23, 55, 87, 119, 279, 311, 343, 375, 534, 790, 20, 151, 183, 215, 247, 407, 439, 662, 918,
            150, 535, 567, 599, 631, 663, 695, 727, 759, 983, 1014}


def ea_of(w, st):
    """Effective address of a memory instruction under state st (dict reg->val), or None."""
    op = w >> 26
    ra = (w >> 16) & 31
    rb = (w >> 11) & 31
    if op in MEM_D:
        d = sext16(w & 0xFFFF) if op < 56 else ((w & 0xFFF) - 0x1000 if w & 0x800 else w & 0xFFF)
        if ra == 0:
            return d & M32
        v = st.get(ra)
        return None if v is None else (v + d) & M32
    if op == 31 or op == 4:
        a = 0 if ra == 0 else st.get(ra)
        b = st.get(rb)
        if a is None or b is None:
            return None
        return (a + b) & M32
    return None


def step(addr, w, st):
    """Transfer function. Returns (new_state, successors_kind)."""
    st = dict(st)
    op = w >> 26
    rd = (w >> 21) & 31
    ra = (w >> 16) & 31
    rb = (w >> 11) & 31
    imm = w & 0xFFFF

    def setr(r, v):
        if v is None:
            st.pop(r, None)
        else:
            st[r] = v & M32

    def get(r):
        return st.get(r)

    if op == 14:  # addi / li
        setr(rd, sext16(imm) if ra == 0 else (None if get(ra) is None else get(ra) + sext16(imm)))
    elif op == 15:  # addis / lis
        setr(rd, (sext16(imm) << 16) if ra == 0 else (None if get(ra) is None else get(ra) + (sext16(imm) << 16)))
    elif op in (24, 25, 26, 27, 28, 29):
        s = get(rd)
        if s is None:
            setr(ra, None)
        else:
            f = {24: lambda: s | imm, 25: lambda: s | (imm << 16), 26: lambda: s ^ imm,
                 27: lambda: s ^ (imm << 16), 28: lambda: s & imm, 29: lambda: s & (imm << 16)}[op]
            setr(ra, f())
    elif op == 21:  # rlwinm
        s = get(rd)
        sh, mb, me = (w >> 11) & 31, (w >> 6) & 31, (w >> 1) & 31
        setr(ra, None if s is None else rotl(s, sh) & mask(mb, me))
    elif op in (20, 23):
        setr(ra, None)
    elif op in (10, 11, 3, 19, 59, 63, 47, 36, 38, 44, 52, 54, 60, 48, 50, 56, 16, 18, 17):
        pass
    elif op in LOAD_D:
        setr(rd, None)
    elif op == 46:  # lmw
        for r in range(rd, 32):
            setr(r, None)
    elif op == 4:
        xo6 = (w >> 1) & 0x3F
        if xo6 in (38, 39):
            setr(ra, None)
    elif op == 31:
        xo = (w >> 1) & 0x3FF
        if xo == 444 and rd == rb:  # mr
            setr(ra, get(rd))
        elif xo in (597, 533):  # lswi/lswx
            st.clear()
        elif xo in ST_NOWRITE_31:
            pass
        elif xo in UPD_31:
            pass  # rA handled below
        elif xo in DEST_RA_31:
            setr(ra, None)
        else:
            setr(rd, None)
    else:
        setr(rd, None)
        setr(ra, None)
    # update forms: rA = EA (computed from the pre-instruction state)
    if (op in UPD_D) or (op == 31 and ((w >> 1) & 0x3FF) in UPD_31) or (op == 4 and ((w >> 1) & 0x3F) in (38, 39)):
        return st, ('upd', ra)
    return st, None


def clobber_call(st):
    for r in [0] + list(range(3, 13)):
        st.pop(r, None)


def is_call(w):
    op = w >> 26
    return w & 1 and (op in (16, 18) or (op == 19 and ((w >> 1) & 0x3FF) in (16, 528)))


def analyze(insns, entry=None, extra_entries=()):
    """insns: list of (addr, word) of one function. entry: the function's address; extra_entries:
    addresses the generated code can jump to directly (switch on ctx.pc), each starting with nothing
    known unless it follows a call. Returns dict addr -> state-before ({} = nothing known)."""
    idx = {a: i for i, (a, _) in enumerate(insns)}
    n = len(insns)
    succ = [[] for _ in range(n)]
    post = [None] * n  # per-edge post state modifier: 'call' -> clobber
    for i, (a, w) in enumerate(insns):
        op = w >> 26
        nxt = i + 1 if i + 1 < n and insns[i + 1][0] == a + 4 else None
        if op == 18:
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            tgt = (li if w & 2 else a + li) & M32
            if w & 1:
                if nxt is not None:
                    succ[i].append((nxt, True))
            elif tgt in idx:
                succ[i].append((idx[tgt], False))
        elif op == 16:
            bd = w & 0xFFFC
            if bd & 0x8000:
                bd -= 0x10000
            tgt = (bd if w & 2 else a + bd) & M32
            bo = (w >> 21) & 31
            always = (bo & 0x14) == 0x14
            if w & 1:
                if nxt is not None:
                    succ[i].append((nxt, True))
            else:
                if tgt in idx:
                    succ[i].append((idx[tgt], False))
                if not always and nxt is not None:
                    succ[i].append((nxt, False))
        elif op == 19 and ((w >> 1) & 0x3FF) in (16, 528):
            bo = (w >> 21) & 31
            always = (bo & 0x14) == 0x14
            if w & 1:
                if nxt is not None:
                    succ[i].append((nxt, True))
            elif not always and nxt is not None:
                succ[i].append((nxt, False))
        elif op == 17:
            if nxt is not None:
                succ[i].append((nxt, True))
        elif op == 19 and ((w >> 1) & 0x3FF) == 50:  # rfi
            pass
        else:
            if nxt is not None:
                succ[i].append((nxt, False))
    TOP = 'TOP'
    before = [None] * n  # None = not reached yet
    starts = [idx.get(entry, 0)]
    for a in extra_entries:
        i = idx.get(a)
        if i is not None and not (i > 0 and insns[i - 1][0] == a - 4 and is_call(insns[i - 1][1])):
            starts.append(i)
    forced = set(starts)
    for i in starts:
        before[i] = {}
    work = collections.deque(starts)
    inq = set(starts)
    while work:
        i = work.popleft()
        inq.discard(i)
        a, w = insns[i]
        st, upd = step(a, w, before[i])
        if upd:
            e = ea_of(w, before[i])
            if e is None:
                st.pop(upd[1], None)
            else:
                st[upd[1]] = e
        for j, via_call in succ[i]:
            s2 = dict(st)
            if via_call:
                clobber_call(s2)
            if j in forced:
                continue
            if before[j] is None:
                before[j] = s2
                ch = True
            else:
                old = before[j]
                new = {r: v for r, v in old.items() if s2.get(r) == v}
                ch = new != old
                before[j] = new
            if ch and j not in inq:
                work.append(j)
                inq.add(j)
    return {insns[i][0]: (before[i] if before[i] is not None else {}) for i in range(n)}, {a: w for a, w in insns}


def main():
    exp, sites, out = sys.argv[1:4]
    import os
    files = sorted((f for f in os.listdir(exp) if re.fullmatch(r'output_\d+\.cpp', f)), key=lambda f: int(f[7:-4]))
    funcs = {}
    cases = collections.defaultdict(list)
    RX_CASE = re.compile(r'^\s*case 0x([0-9A-Fa-f]+): goto loc_')
    for fn in files:
        cur = None
        for line in open(os.path.join(exp, fn)):
            m = RX_FUNC.match(line)
            if m:
                cur = m.group(1)
                funcs[cur] = []
                continue
            m = RX_CASE.match(line)
            if m and cur:
                cases[cur].append(int(m.group(1), 16))
                continue
            m = RX_PC.match(line)
            if m and cur:
                funcs[cur].append((int(m.group(1), 16), int(m.group(2), 16)))
    states = {}
    words = {}
    for f, ins in funcs.items():
        if not ins:
            continue
        # dedupe + sort by address (each instruction once)
        ins = sorted(set(ins))
        m = re.search(r'([0-9A-F]{8})$', f)
        # resume points (switch on ctx.pc after a call or an interrupt poll) continue the same guest
        # execution with the same registers, so they are not extra entries; bctr targets are never
        # reached by the propagation and start with nothing known.
        s, wd = analyze(ins, int(m.group(1), 16) if m else None)
        for a in s:
            states[(f, a)] = s[a]
            words[(f, a)] = wd[a]
    cnt = collections.Counter()
    with open(out, 'w') as o:
        o.write('id\tpc\tkind\tstatic\tea\n')
        for line in list(open(sites))[1:]:
            i, pc, op, kind, func, fn, ln = line.rstrip('\n').split('\t')
            a = int(pc, 16)
            key = (func, a)
            cls, eas = 'unknown', ''
            if key in states:
                w = words[key]
                e = ea_of(w, states[key])
                if e is not None:
                    # lmw/stmw/psq W=0 touch several words: classify the start
                    cls, eas = region(e), f'{e:08X}'
            cnt[cls] += 1
            o.write(f'{i}\t{pc}\t{kind}\t{cls}\t{eas}\n')
    for k, v in cnt.most_common():
        print(f'{k:14s} {v}')


if __name__ == '__main__':
    main()
