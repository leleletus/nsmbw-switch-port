#!/usr/bin/env python3
"""apply_r9_axfx_fast.py <export_dir>

R9 cpucode post-pass, after apply_r9_axfx.py (needs its nwii_r9_axfx_native).

func_801A3880 is the AXFX reverb callback (HandleReverb-like: 96 samples x 3
channels; early-reflection taps, optional pre-delay, 3 combs, 2 all-passes, a
low-pass, a per-channel all-pass, cross-talk, in-place output and optional
send output). The native body of apply_r9_axfx.py is the generated code line by
line without ctx; it still re-reads every struct field and pointer per sample,
goes through the MemView tests for every access and keeps the guest's stack
temporaries (int<->float conversions through the stack). This pass adds
nwii_r9_axfx_fast, which runs samples 0..94 of the sample loop with the same
float operations in the same order (every result rounded to single like
fadds/fsubs/fmuls; operand C of fmuls is always a single-precision value here
(lfs or a single op), so Force25Bit leaves it unchanged and is not repeated),
and then lets the native body run the last sample (95) as before. So:
  - guest memory: every delay line, index, filter state, in-place sample and
    send output gets the same final value (the order of independent stores
    changes, which is why all the regions are checked to be disjoint first);
  - the guest stack temporaries (r1+20..r1+76) are all rewritten by sample 95,
    the pointers the loop keeps on the stack (r1+8.., r1+44..) are written back;
  - registers: sample 95 runs in the native body, so every register, ps1, CR,
    CTR and pc at loc_801A3D8C is what the generated code leaves; inst_count gets
    the same per-sample sums (the branch conditions are loop-invariant).
Preconditions (else the native body runs all 96 samples, as before): no
NWII_WATCH; the struct, the constants, the stack frame and every buffer are plain
MEM1/MEM2 RAM; every index is below its size; no written region overlaps any
other region (physical addresses). NWII_AXFX_FAST=0 turns it off.
NWII_AXFX_VERIFY (apply_r9_axfx.py) compares the native body, fast part included,
with the generated code (all guest RAM and registers).
Applied once (// [r9axfxfast]).
"""
import os, sys

MARK = '// [r9axfxfast]'

FAST = r'''
// [r9axfxfast] samples 0..94 of the reverb loop (perf/apply_r9_axfx_fast.py)
namespace {
struct R9AxRgn { uint32_t p, n; bool w; };
uint64_t g_r9_axfx_fast_runs = 0, g_r9_axfx_fast_skips = 0;
}
static NWII_NOINLINE bool nwii_r9_axfx_fast(nwii::runtime::MemView& mv, uint32_t g0, uint32_t g1, uint32_t g2,
                                            uint32_t g4, uint32_t g29, uint32_t& g3, uint32_t& g5, uint32_t& g6,
                                            double f0, double f1, double f3, double f4, double f5, double f6,
                                            uint64_t& ic) {
  using nwii::runtime::MMU;
  static const bool on = [] { const char* e = NWII_ENV("NWII_AXFX_FAST"); return !(e && e[0] == '0'); }();
  if (!on || mv.watch) return false;
  auto pa = [](uint32_t a) -> uint32_t { return a & 0x3FFFFFFFu; };
  auto okr = [](uint32_t p, uint64_t n) -> bool {
    return n > 0 && ((p >= 0x10u && (uint64_t)p + n <= MMU::kMem1Size) ||
                     (p >= 0x10000000u && (uint64_t)p + n <= 0x10000000ull + MMU::kMem2Size));
  };
  auto hp = [&](uint32_t p) -> uint8_t* { return p < 0x10000000u ? mv.b1 + p : mv.b2 + (p - 0x10000000u); };
  auto rd32 = [](const uint8_t* q) -> uint32_t { uint32_t v; std::memcpy(&v, q, 4); return __builtin_bswap32(v); };
  auto wr32 = [](uint8_t* q, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(q, &v, 4); };
  auto rdf = [&](const uint8_t* q) -> double { const uint32_t b = rd32(q); float f; std::memcpy(&f, &b, 4); return (double)f; };
  auto wrf = [&](uint8_t* q, double d) { wr32(q, nwii::runtime::ppc_single_bits(d)); };
  // stfs + lfs of the same slot (the guest does this through its stack)
  auto rt = [](double d) -> double { return (double)nwii::runtime::f32_from(nwii::runtime::ppc_single_bits(d)); };
  auto sp = [](double d) -> double { return nwii::runtime::ppc_single(d); };
  auto fct = [](double d) -> uint32_t { return (uint32_t)nwii::runtime::ppc_fctiw_image(nwii::runtime::ppc_fctiwz(d), d); };

  R9AxRgn rg[48];
  int nr = 0;
  auto add = [&](uint32_t vaddr, uint64_t n, bool w) -> bool {
    const uint32_t p = pa(vaddr);
    if (!okr(p, n)) return false;
    rg[nr++] = {p, (uint32_t)n, w};
    return true;
  };
  // the struct (offsets 0..327), the SDA2 constants (r2-20804..r2-20776), the stack frame slots
  if (!add(g4, 328, true) || !add(g2 - 20804u, 28, false) || !add(g1 + 8u, 72, true)) return false;
  uint8_t* const R = hp(pa(g4));
  uint8_t* const S = hp(pa(g1 + 8u)) - 8;  // S + off = guest r1 + off
  const uint32_t send = rd32(R + 312), outp = rd32(R + 316);
  const uint32_t esz = rd32(R + 24), pdn = rd32(R + 60);
  uint32_t ei[3] = {rd32(R + 12), rd32(R + 16), rd32(R + 20)};
  uint32_t pdi = rd32(R + 56);
  uint32_t ci[3] = {rd32(R + 104), rd32(R + 108), rd32(R + 112)};
  const uint32_t cn[3] = {rd32(R + 116), rd32(R + 120), rd32(R + 124)};
  uint32_t ai[2] = {rd32(R + 176), rd32(R + 180)};
  const uint32_t an[2] = {rd32(R + 184), rd32(R + 188)};
  if (esz == 0 || ei[0] >= esz || ei[1] >= esz || ei[2] >= esz) return false;
  if (pdn != 0 && pdi >= pdn) return false;
  for (int k = 0; k < 3; ++k) if (ci[k] >= cn[k]) return false;
  for (int k = 0; k < 2; ++k) if (ai[k] >= an[k]) return false;
  uint8_t *in[3], *sd[3] = {}, *E[3], *PD[3] = {}, *C[3][3], *A[3][2], *B3[3], *ou[3] = {};
  uint32_t b3i[3], b3n[3];
  const uint32_t outv[3] = {g3, g5, g6};
  for (int ch = 0; ch < 3; ++ch) {
    const uint32_t vin = rd32(S + 44 + 4 * ch);
    if (!add(vin, 96 * 4, true)) return false;
    in[ch] = hp(pa(vin));
    if (send != 0) {
      const uint32_t vs = rd32(S + 8 + 4 * ch);
      if (!add(vs, 96 * 4, false)) return false;
      sd[ch] = hp(pa(vs));
    }
    if (outp != 0) {
      if (!add(outv[ch], 96 * 4, true)) return false;
      ou[ch] = hp(pa(outv[ch]));
    }
    const uint32_t ve = rd32(R + 0 + 4 * ch);
    if (!add(ve, 4ull * esz, true)) return false;
    E[ch] = hp(pa(ve));
    if (pdn != 0) {
      const uint32_t vp = rd32(R + 44 + 4 * ch);
      if (!add(vp, 4ull * pdn, true)) return false;
      PD[ch] = hp(pa(vp));
    }
    for (int k = 0; k < 3; ++k) {
      const uint32_t vc = rd32(R + 68 + 12 * ch + 4 * k);
      if (!add(vc, 4ull * cn[k], true)) return false;
      C[ch][k] = hp(pa(vc));
    }
    for (int k = 0; k < 2; ++k) {
      const uint32_t va = rd32(R + 152 + 8 * ch + 4 * k);
      if (!add(va, 4ull * an[k], true)) return false;
      A[ch][k] = hp(pa(va));
    }
    const uint32_t vb = rd32(R + 200 + 4 * ch);
    b3i[ch] = rd32(R + 212 + 4 * ch);
    b3n[ch] = rd32(R + 224 + 4 * ch);
    if (b3i[ch] >= b3n[ch] || !add(vb, 4ull * b3n[ch], true)) return false;
    B3[ch] = hp(pa(vb));
  }
  for (int i = 0; i < nr; ++i)
    for (int j = i + 1; j < nr; ++j)
      if ((rg[i].w || rg[j].w) && (uint64_t)rg[i].p < (uint64_t)rg[j].p + rg[j].n &&
          (uint64_t)rg[j].p < (uint64_t)rg[i].p + rg[i].n)
        return false;

  // loop-invariant values (struct fields the loop only reads, constants)
  const double k32 = rdf(R + 32), k36 = rdf(R + 36), k40 = rdf(R + 40);
  const double kc[3] = {rdf(R + 140), rdf(R + 144), rdf(R + 148)};
  const double k320 = rdf(R + 320), k324 = rdf(R + 324);
  const double z = rdf(hp(pa(g2 - 20804u)));  // lfs f10, -20804(r2)
  double lp[3] = {rdf(R + 252), rdf(R + 256), rdf(R + 260)};
  const uint64_t hi = (uint64_t)g0 << 32;
  double acc[3];
  for (uint32_t n = 0; n < 95; ++n) {
    for (int ch = 0; ch < 3; ++ch) {
      // input sample (+ send) as a float: (0x43300000:(x ^ 0x80000000)) - f0, fsubs
      uint32_t x = rd32(in[ch] + 4 * n);
      if (send != 0) x = x + rd32(sd[ch] + 4 * n);
      const double f11 = sp(nwii::runtime::f64_from(hi | (uint32_t)(x ^ 0x80000000u)) - f0);
      // early reflections
      uint8_t* const e = E[ch];
      double f8 = rdf(e + 4 * ei[0]);
      double f2 = rdf(e + 4 * ei[1]);
      f8 = sp(k32 * f8);
      double f9 = rdf(e + 4 * ei[2]);
      f2 = sp(k36 * f2);
      wrf(e + 4 * ei[2], f11);
      double f7 = sp(k40 * f9);
      f2 = sp(f8 + f2);
      f2 = sp(f7 + f2);
      // pre-delay
      if (pdn != 0) {
        f9 = rdf(PD[ch] + 4 * pdi);
        wrf(PD[ch] + 4 * pdi, f11);
      } else {
        f9 = f11;
      }
      // combs
      double f10 = z;
      for (int k = 0; k < 3; ++k) {
        uint8_t* const q = C[ch][k] + 4 * ci[k];
        f8 = rdf(q);
        f7 = sp(f8 * kc[k]);
        f10 = sp(f10 + f8);
        f7 = sp(f9 + f7);
        wrf(q, f7);
      }
      // all-passes
      {
        uint8_t* const q = A[ch][0] + 4 * ai[0];
        f8 = rdf(q);
        f7 = sp(f8 * f3);
        f7 = sp(f10 + f7);
        wrf(q, f7);
        f7 = sp(f7 * f3);
        f8 = sp(f8 - f7);
      }
      {
        uint8_t* const q = A[ch][1] + 4 * ai[1];
        f9 = rdf(q);
        f7 = sp(f9 * f3);
        f7 = sp(f8 + f7);
        wrf(q, f7);
        f8 = sp(f7 * f3);
      }
      // low-pass
      f7 = lp[ch];
      f8 = sp(f9 - f8);
      f7 = sp(f1 * f7);
      f8 = sp(f4 * f8);
      f9 = sp(f8 + f7);
      lp[ch] = rt(f9);
      // per-channel all-pass
      {
        uint8_t* const q = B3[ch] + 4 * b3i[ch];
        f8 = rdf(q);
        f7 = sp(f8 * f3);
        f7 = sp(f9 + f7);
        wrf(q, f7);
        f7 = sp(f7 * f3);
        if (++b3i[ch] >= b3n[ch]) b3i[ch] = g29;
        f7 = sp(f8 - f7);
      }
      f7 = rt(f7);
      f7 = sp(f7 * f5);
      f2 = sp(f7 + f2);
      acc[ch] = rt(f2);
    }
    // cross-talk and outputs (L = ch0, R = ch1, S = ch2)
    const double f10 = acc[0], f12 = acc[1], f11 = acc[2];
    double f2 = sp(f12 + f11);
    double f7 = sp(f10 + f11);
    double f9 = sp(f2 * f6);
    double f8 = sp(f7 * f6);
    const double f13 = sp(f10 + f12);
    f9 = sp(f10 + f9);
    f8 = sp(f12 + f8);
    f7 = sp(f13 * f6);
    f7 = sp(f11 + f7);
    // (stfs f9/f8/f7 to r1+32..40 only feed the stack: the products below use the registers)
    wr32(in[0] + 4 * n, fct(sp(f9 * k320)));
    wr32(in[1] + 4 * n, fct(sp(f8 * k320)));
    wr32(in[2] + 4 * n, fct(sp(f7 * k320)));
    if (outp != 0) {
      wr32(ou[0] + 4 * n, fct(sp(f9 * k324)));
      wr32(ou[1] + 4 * n, fct(sp(f8 * k324)));
      wr32(ou[2] + 4 * n, fct(sp(f7 * k324)));
    }
    // indices
    for (int k = 0; k < 3; ++k) if (++ei[k] >= esz) ei[k] = g29;
    if (pdn != 0 && ++pdi >= pdn) pdi = g29;
    for (int k = 0; k < 3; ++k) if (++ci[k] >= cn[k]) ci[k] = g29;
    for (int k = 0; k < 2; ++k) if (++ai[k] >= an[k]) ai[k] = g29;
  }
  // write back the state the loop keeps in the struct and on the stack
  wr32(R + 12, ei[0]); wr32(R + 16, ei[1]); wr32(R + 20, ei[2]);
  if (pdn != 0) wr32(R + 56, pdi);
  wr32(R + 104, ci[0]); wr32(R + 108, ci[1]); wr32(R + 112, ci[2]);
  wr32(R + 176, ai[0]); wr32(R + 180, ai[1]);
  for (int ch = 0; ch < 3; ++ch) {
    wrf(R + 252 + 4 * ch, lp[ch]);
    wr32(R + 212 + 4 * ch, b3i[ch]);
    wr32(S + 44 + 4 * ch, rd32(S + 44 + 4 * ch) + 95 * 4);
    if (send != 0) wr32(S + 8 + 4 * ch, rd32(S + 8 + 4 * ch) + 95 * 4);
  }
  if (outp != 0) { g3 += 95 * 4; g5 += 95 * 4; g6 += 95 * 4; }
  // inst_count of 95 samples, block by block as the generated code counts them
  const uint64_t per_ch = 16 + (send == 0 ? 7 : 0) + 28 + (pdn == 0 ? 1 : 0) + 68 + 11;
  const uint64_t per_sample = 8 + 3 * per_ch + 66 + 7 + 7 + 8 + 9 + 7 + 7 + 8 + 7 + 7 + 3;
  ic += 95 * per_sample;
  return true;
}
'''

HOOK_OLD = '''    f0 = mv_.read_f64(g2 + -20784); // lfd
    g0 = g0 | 0; // ori
loc_801A3940: ;
'''
HOOK_NEW = '''    f0 = mv_.read_f64(g2 + -20784); // lfd
    g0 = g0 | 0; // ori
    if (nwii_r9_axfx_fast(mv_, g0, g1, g2, g4, g29, g3, g5, g6, f0, f1, f3, f4, f5, f6, ic)) {  ''' + MARK + '''
      ++g_r9_axfx_fast_runs;
      g31 = 95;  // the native body runs the last sample
    } else {
      ++g_r9_axfx_fast_skips;
    }
loc_801A3940: ;
'''
REPORT_OLD = '''  std::cout << "[R9] axfx native=" << v.native << " generated(EE set)=" << v.fallback'''
REPORT_NEW = '''  std::cout << "[R9] axfx native=" << v.native << " generated(EE set)=" << v.fallback
            << " fast=" << g_r9_axfx_fast_runs << " fast_skipped=" << g_r9_axfx_fast_skips  ''' + MARK


def main():
    d = sys.argv[1]
    p = os.path.join(d, 'output_24.cpp')
    s = open(p).read()
    if MARK in s:
        sys.exit('apply_r9_axfx_fast: already applied')
    if '// [r9axfx]' not in s:
        sys.exit('apply_r9_axfx_fast: apply_r9_axfx.py first')
    anchor = 'static NWII_NOINLINE void nwii_r9_axfx_native(CPUContext& ctx) {'
    for old in (anchor, HOOK_OLD, REPORT_OLD):
        if s.count(old) != 1:
            sys.exit('apply_r9_axfx_fast: anchor not found once: ' + old.splitlines()[0])
    # the fast part needs these exact live-ins at loc_801A3940 (checked in the native body text)
    nat = s[s.index(anchor):]
    nat = nat[:nat.index('\n}\n')]
    for need in ('g31 = 0; // li r31, 0', 'g29 = 0; // li r29, 0', 'g30 = 3; // li r30, 3',
                 'g0 = 1127219200; // lis', 'NWII_R9_FSUBSW(4, 0, 1);', 'NWII_R9_FMULSW(5, 5, 3);',
                 'NWII_R9_FMULSW(6, 2, 0);', 'NWII_R9_FILLW(3, (double)mv_.read_f32(g4 + 248)); // lfs',
                 'NWII_R9_FILLW(1, (double)mv_.read_f32(g4 + 264)); // lfs'):
        if need not in nat:
            sys.exit('apply_r9_axfx_fast: native body differs from the one this pass was written for: ' + need)
    s = s.replace(anchor, FAST.lstrip('\n') + '\n' + anchor, 1)
    s = s.replace(HOOK_OLD, HOOK_NEW, 1)
    s = s.replace(REPORT_OLD, REPORT_NEW, 1)
    open(p, 'w').write(s)
    print('apply_r9_axfx_fast: applied to', p)


if __name__ == '__main__':
    main()
