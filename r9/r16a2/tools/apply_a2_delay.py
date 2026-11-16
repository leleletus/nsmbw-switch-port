#!/usr/bin/env python3
"""apply_a2_delay.py <output_24.cpp>   (r16a2, front A2)

Native body for func_801A5C10, the AXFX delay callback of SMNE01 (integer only: 96 samples x
3 channels, three delay lines; ~0.26 % of the CPU thread on the console, r13b). The same recipe as
r9/cpucode/perf/apply_r9_axfx.py for the reverb: the body of the function from loc_801A5C24
(right after the _savegpr_20 call) to loc_801A5E70 (right before the _restgpr_20 call) is copied
into nwii_a2_delay_native() mechanically, guest registers in locals:
  ctx.gpr[N] -> gN, ctx.cr[0].x -> c0x, ctx.xer -> xer, ctx.ctr -> ctr, ctx.inst_count += k ->
  ic += k; guest memory through the same MemView accessors (same fast and slow paths, same order).
At the exit every register the body writes goes back to ctx, so at loc_801A5E70 ctx holds what
the generated code leaves there. The one interrupt poll (bdnz at 0x801A5E6C) is left out; the
native body only runs with MSR[EE] clear (the AX callback runs from the DSP interrupt), where a
poll cannot deliver an interrupt and only advances the event scheduler (that then happens at the
caller's next poll, a few microseconds later).
Switches: NWII_AXFX_DELAY_NATIVE=0 turns it off. NWII_AXFX_DVERIFY=n (test): every n-th call
runs the native body on a snapshot of guest RAM and registers, restores it, runs the generated
code and compares everything ("[A2] delay verify ..."). [A2] delay line every 6000 calls.
Applied once (mark // [a2delay]); refuses if the function text is not the expected one.
"""
import hashlib, re, sys

FUNC = 'func_801A5C10'
START = 'loc_801A5C24'
END = 'loc_801A5E70'
MARK = '// [a2delay]'
EXPECTED_SHA = "985bcf3f4c9045a879cb730e190bd5930b758fe7c195eed5e6d37e0d2f93ba43"  # r9/r13 export (after the fastmem pass)
POLL_PC = re.compile(r'^ctx\.pc = (0x[0-9A-F]+);$')


def find_function(text):
    m = re.search(r'^void %s\(CPUContext& ctx\) \{\n' % FUNC, text, re.M)
    if not m:
        return None
    return m.start(), text.index('\n}\n', m.end()) + 3


def translate(body):
    out, gw, gr = [], set(), set()
    crw = xerw = ctrw = False
    i, n = 0, len(body)
    while i < n:
        line = body[i].strip()
        code, _, comment = line.partition(' //')
        code = code.strip()
        i += 1
        if not code or code.startswith('//'):
            continue
        m = POLL_PC.match(code)
        if m and i < n and body[i].strip() == 'if (process_pending_callbacks(ctx)) return;':
            out.append('    pc = %s;  // interrupt poll of the generated code (MSR[EE] is clear here)' % m.group(1))
            i += 1
            continue
        if code == 'goto %s;' % END:
            out.append('    goto nwii_exit;')
            continue
        for mm in re.finditer(r'ctx\.gpr\[(\d+)\]\s*=(?!=)', code):
            gw.add(int(mm.group(1)))
        for mm in re.finditer(r'ctx\.gpr\[(\d+)\]', code):
            gr.add(int(mm.group(1)))
        if re.search(r'ctx\.cr\[0\]\.(lt|gt|eq|so)\s*=(?!=)', code):
            crw = True
        if re.search(r'ctx\.xer\s*=(?!=)', code):
            xerw = True
        if re.search(r'ctx\.ctr\s*(=(?!=)|--|\+\+)', code):
            ctrw = True
        code = re.sub(r'ctx\.gpr\[(\d+)\]', r'g\1', code)
        code = re.sub(r'ctx\.cr\[0\]\.(lt|gt|eq|so)', r'c0\1', code)
        code = code.replace('ctx.xer', 'xer').replace('ctx.ctr', 'ctr')
        code = re.sub(r'^ctx\.inst_count \+= (\d+);$', r'ic += \1;', code)
        if 'ctx' in code:
            sys.exit('apply_a2_delay: statement not handled: ' + line)
        if re.match(r'^loc_[0-9A-F]+: ;$', code):
            out.append(code)
        else:
            out.append('    ' + code + ((' //' + comment) if comment else ''))
    return out, gw, gr, crw, xerw, ctrw


def build_native(body):
    out, gw, gr, crw, xerw, ctrw = translate(body)
    used = set(re.findall(r'goto (loc_[0-9A-F]+);', '\n'.join(out)))
    out = [l for l in out if not re.match(r'^loc_[0-9A-F]+: ;$', l) or l[:-3] in used]
    L = [MARK + ' native body of %s from %s to %s (r9/r16a2/tools/apply_a2_delay.py)' % (FUNC, START, END),
         'static NWII_NOINLINE void nwii_a2_delay_native(CPUContext& ctx) {',
         '    nwii::runtime::MemView mv_(ctx.mmu);']
    for r in sorted(gw | gr):
        L.append('    uint32_t g%d = ctx.gpr[%d];' % (r, r))
    L.append('    bool c0lt = ctx.cr[0].lt, c0gt = ctx.cr[0].gt, c0eq = ctx.cr[0].eq, c0so = ctx.cr[0].so;')
    L.append('    uint32_t ctr = ctx.ctr, pc = ctx.pc, xer = ctx.xer;')
    L.append('    uint64_t ic = 0;')
    L.extend(out)
    L.append('nwii_exit:')
    for r in sorted(gw):
        L.append('    ctx.gpr[%d] = g%d;' % (r, r))
    if crw:
        L.append('    ctx.cr[0].lt = c0lt; ctx.cr[0].gt = c0gt; ctx.cr[0].eq = c0eq; ctx.cr[0].so = c0so;')
    if ctrw:
        L.append('    ctx.ctr = ctr;')
    if xerw:
        L.append('    ctx.xer = xer;')
    L.append('    ctx.pc = pc;')
    L.append('    ctx.inst_count += ic;')
    L.append('    (void)c0lt; (void)c0gt; (void)c0eq; (void)c0so;')
    L.append('}')
    return L


HOOK = r'''
// Entry from func_801A5C10 at loc_801A5C24. true: the native body ran and ctx is the state of
// loc_801A5E70. false: run the generated code (MSR[EE] set, off, or a verify round).
namespace {
struct A2DelayVerify {
  bool pending = false;
  uint64_t calls = 0, ok = 0, bad = 0, native = 0, fallback = 0;
  std::vector<uint8_t> s1, s2;
  uint64_t h1 = 0, h2 = 0;
  R9AxfxRegs regs;
};
A2DelayVerify& a2_delay_state() { static A2DelayVerify v; return v; }
}  // namespace

static NWII_NOINLINE void nwii_a2_delay_report(const A2DelayVerify& v) {
  std::printf("[A2] delay native=%llu generated(EE set)=%llu verify ok=%llu bad=%llu\n",
              (unsigned long long)v.native, (unsigned long long)v.fallback, (unsigned long long)v.ok,
              (unsigned long long)v.bad);
  std::fflush(stdout);
}

static NWII_NOINLINE bool nwii_a2_delay_verify_begin(CPUContext& ctx) {
  A2DelayVerify& v = a2_delay_state();
  v.s1.resize(ctx.mmu.mem1.size());
  v.s2.resize(ctx.mmu.mem2.size());
  std::memcpy(v.s1.data(), ctx.mmu.mem1.data(), v.s1.size());
  std::memcpy(v.s2.data(), ctx.mmu.mem2.data(), v.s2.size());
  const R9AxfxRegs before = r9_axfx_regs(ctx);
  nwii_a2_delay_native(ctx);
  v.h1 = r9_axfx_hash(ctx.mmu.mem1);
  v.h2 = r9_axfx_hash(ctx.mmu.mem2);
  v.regs = r9_axfx_regs(ctx);
  std::memcpy(ctx.mmu.mem1.data(), v.s1.data(), v.s1.size());
  std::memcpy(ctx.mmu.mem2.data(), v.s2.data(), v.s2.size());
  r9_axfx_set_regs(ctx, before);
  v.pending = true;
  return false;
}

static NWII_NOINLINE void nwii_a2_delay_verify_end(CPUContext& ctx) {
  A2DelayVerify& v = a2_delay_state();
  v.pending = false;
  const R9AxfxRegs now = r9_axfx_regs(ctx);
  bool good = std::memcmp(&now, &v.regs, sizeof now) == 0;
  if (!good && v.bad < 20) {
    for (int i = 0; i < 32; ++i)
      if (now.gpr[i] != v.regs.gpr[i]) std::printf("[A2] delay verify r%d gen=0x%x native=0x%x\n", i, now.gpr[i], v.regs.gpr[i]);
    if (std::memcmp(now.cr, v.regs.cr, sizeof now.cr)) std::printf("[A2] delay verify cr differs\n");
    if (now.xer != v.regs.xer) std::printf("[A2] delay verify xer gen=0x%x native=0x%x\n", now.xer, v.regs.xer);
    if (now.ctr != v.regs.ctr || now.pc != v.regs.pc || now.inst_count != v.regs.inst_count || now.lr != v.regs.lr)
      std::printf("[A2] delay verify ctr/pc/lr/inst gen=%u/0x%x/0x%x/%llu native=%u/0x%x/0x%x/%llu\n", now.ctr, now.pc,
                  now.lr, (unsigned long long)now.inst_count, v.regs.ctr, v.regs.pc, v.regs.lr,
                  (unsigned long long)v.regs.inst_count);
  }
  const uint64_t g1 = r9_axfx_hash(ctx.mmu.mem1), g2 = r9_axfx_hash(ctx.mmu.mem2);
  if (g1 != v.h1 || g2 != v.h2) {
    good = false;
    if (v.bad < 20) std::printf("[A2] delay verify guest RAM differs (MEM1 %s, MEM2 %s)\n", g1 != v.h1 ? "differs" : "same",
                                g2 != v.h2 ? "differs" : "same");
  }
  if (good) ++v.ok; else ++v.bad;
  if ((v.ok + v.bad) % 50 == 1 || !good) nwii_a2_delay_report(v);
}

static inline bool nwii_a2_delay_801A5C24(CPUContext& ctx) {
  static const bool on = [] { const char* e = NWII_ENV("NWII_AXFX_DELAY_NATIVE"); return !(e && e[0] == '0'); }();
  static const unsigned long every = [] { const char* e = NWII_ENV("NWII_AXFX_DVERIFY"); return e ? std::strtoul(e, nullptr, 10) : 0ul; }();
  A2DelayVerify& v = a2_delay_state();
  if (__builtin_expect(!on || (ctx.msr & 0x8000u), 0)) {
    ++v.fallback;
    return false;
  }
  ++v.calls;
  if (__builtin_expect(every != 0, 0) && v.calls % every == 0)
    return nwii_a2_delay_verify_begin(ctx);
  if (__builtin_expect(++v.native % 6000 == 0, 0)) nwii_a2_delay_report(v);
  nwii_a2_delay_native(ctx);
  return true;
}
'''


def main():
    p = sys.argv[1]
    text = open(p).read()
    span = find_function(text)
    if not span:
        sys.exit('apply_a2_delay: %s not found' % FUNC)
    a, b = span
    func = text[a:b]
    sha = hashlib.sha256(func.encode()).hexdigest()
    if len(sys.argv) > 2 and sys.argv[2] == '--sha':
        print(sha)
        return
    if MARK in text:
        sys.exit('apply_a2_delay: already applied')
    if EXPECTED_SHA and sha != EXPECTED_SHA:
        sys.exit('apply_a2_delay: unexpected function text (sha256 %s)' % sha)
    if 'r9_axfx_regs(' not in text[:a]:
        sys.exit('apply_a2_delay: needs the reverb pass (r9_axfx_regs) earlier in the file')
    lines = func.split('\n')
    si = lines.index(START + ': ;')
    ei = lines.index(END + ': ;')
    native = build_native(lines[si + 1:ei])
    lines.insert(si + 1, '    if (nwii_a2_delay_801A5C24(ctx)) goto %s;  %s' % (END, MARK))
    ei = lines.index(END + ': ;')
    lines.insert(ei + 1, '    if (__builtin_expect(a2_delay_state().pending, 0)) nwii_a2_delay_verify_end(ctx);  ' + MARK)
    new = '\n'.join(native) + '\n' + HOOK + '\n' + '\n'.join(lines)
    open(p, 'w').write(text[:a] + new + text[b:])
    print('apply_a2_delay: native body of %d lines' % len(native))


if __name__ == '__main__':
    main()
