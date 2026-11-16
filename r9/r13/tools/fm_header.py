#!/usr/bin/env python3
"""R12fm: rewrite r12fm/nWiiRuntime/include/runtime/cpu_context.h (a copy of r10c's) for fastmem.
Idempotent (marker), checks the sha256 of the r10c original before editing."""
import hashlib, sys, re
P = sys.argv[1]
src = open(P).read()
MARK = '// [r12fm-header]'
if MARK in src:
    print('already applied'); sys.exit(0)
EXPECT = sys.argv[2] if len(sys.argv) > 2 else None
h = hashlib.sha256(src.encode()).hexdigest()
if EXPECT and h != EXPECT:
    print('sha256 mismatch', h); sys.exit(3)

def rep(old, new, count=1):
    global src
    n = src.count(old)
    if n != count:
        print('pattern count', n, '!=', count, ':', old[:80]); sys.exit(4)
    src = src.replace(old, new)

# 1. fastmem block + MemBlock before the MMU
rep('''extern uint32_t g_watch_addr NWII_HIDDEN;
''', '''extern uint32_t g_watch_addr NWII_HIDDEN;

''' + MARK + r''' R12 fastmem (r9/r12fm/LEEME.md, r9/r11c/DISENO_FASTMEM.md).
// MMU::mem1/mem2/lc are views (pointer + size) of canonical blocks that fm_alloc() maps a
// second time into a 4 GiB window at g_fm_base (MEM1 at 0x00/0x40/0x80/0xC0000000, MEM2 at
// 0x10/0x50/0x90/0xD0000000, LC at 0xE0000000; everything else unmapped). With NWII_FASTMEM
// the generated code accesses guest memory as one asm load/store at g_fm_base + ea, listed in
// section nwii_fm_tab ({pc - &entry, kind | site << 8}); an access to anything that is not RAM
// faults and the SIGSEGV handler (src/core/fastmem_impl.inc) emulates it with MMU::*_slow.
#ifndef NWII_FASTMEM
#if defined(__x86_64__) && defined(__linux__)
#define NWII_FASTMEM 1
#else
#define NWII_FASTMEM 0
#endif
#endif
struct MemBlock {
  uint8_t *p = nullptr;
  size_t n = 0;
  uint8_t *data() { return p; }
  const uint8_t *data() const { return p; }
  size_t size() const { return n; }
  bool empty() const { return n == 0; }
  uint8_t &operator[](size_t i) { return p[i]; }
  const uint8_t &operator[](size_t i) const { return p[i]; }
  uint8_t *begin() { return p; }
  uint8_t *end() { return p + n; }
  const uint8_t *begin() const { return p; }
  const uint8_t *end() const { return p + n; }
};
extern uint8_t *g_fm_base NWII_HIDDEN;  // 4 GiB window (nullptr without fastmem)
// Allocates the three blocks (sizes include the 8 bytes of padding of the old vectors),
// zeroed; with NWII_FASTMEM also the window, its views and the fault handler.
void fm_alloc(MemBlock &mem1, MemBlock &mem2, MemBlock &lc);
enum FmKind : uint32_t { FM_R8 = 1, FM_R16, FM_R32, FM_R64, FM_RF32, FM_RF64,
                         FM_W8, FM_W16, FM_W32, FM_W64, FM_WF32, FM_WF64 };
''')
rep('''  std::vector<uint8_t> mem1;
  std::vector<uint8_t> mem2;''', '''  MemBlock mem1;
  MemBlock mem2;''')
rep('''  std::vector<uint8_t> lc;

  MMU() {
    mem1.resize(24 * 1024 * 1024 + 8, 0);
    mem2.resize(64 * 1024 * 1024 + 8, 0);
    lc.resize(kLCSize + 8, 0);
  }''', '''  MemBlock lc;

  MMU() { fm_alloc(mem1, mem2, lc); }
  MMU(const MMU &) = delete;
  MMU &operator=(const MMU &) = delete;''')

# 2. MemView: template accessors (site id), _io variants, fastmem flavour
i0 = src.index('struct MemView {')
i1 = src.index('#undef NWII_MV_ID\n};\n', i0) + len('#undef NWII_MV_ID\n};\n')
old = src[i0:i1]
# keep old body as the non-fastmem flavour, renaming accessors to *_ref and adding templates
body = old
body = body.replace('struct MemView {', 'struct MemView {', 1)
for nm in ['read8', 'read16', 'read32', 'read64', 'read_f32', 'read_f64', 'write8', 'write16', 'write32',
           'write64', 'write_f32', 'write_f64']:
    body = re.sub(r'NWII_FORCEINLINE (\w+) ' + nm + r'\(', r'NWII_FORCEINLINE \1 ' + nm + '_ref(', body)
body = body.replace('uint32_t val = read32(addr); float f;', 'uint32_t val = read32_ref(addr); float f;')
tmpl = r'''
  // ---- R12 fastmem: site-numbered accessors (apply_r12_fastmem.py) ---------------------
  // mv_.read32<ID>(ea): plain access (fastmem asm or the MemView test above);
  // mv_.read32_io(ea): a listed MMIO site -> the MMU slow path directly (no check, no fault).
#define NWII_FM_IO(T, nm) NWII_FORCEINLINE T nm##_io(uint32_t addr) { return m.nm##_slow(addr); }
#define NWII_FM_IOW(T, nm) NWII_FORCEINLINE void nm##_io(uint32_t addr, T value) { m.nm##_slow(addr, value); }
  NWII_FM_IO(uint8_t, read8) NWII_FM_IO(uint16_t, read16) NWII_FM_IO(uint32_t, read32)
  NWII_FM_IO(uint64_t, read64) NWII_FM_IO(double, read_f64)
  NWII_FORCEINLINE float read_f32_io(uint32_t addr) { uint32_t v = m.read32_slow(addr); float f; std::memcpy(&f, &v, 4); return f; }
  NWII_FM_IOW(uint8_t, write8) NWII_FM_IOW(uint16_t, write16) NWII_FM_IOW(uint32_t, write32)
  NWII_FM_IOW(uint64_t, write64) NWII_FM_IOW(float, write_f32) NWII_FM_IOW(double, write_f64)
#undef NWII_FM_IO
#undef NWII_FM_IOW
#if NWII_FASTMEM && defined(__x86_64__)
  uint8_t *const fb = g_fm_base;
  struct FmB1 { uint8_t x[1]; } __attribute__((may_alias));
  struct FmB2 { uint8_t x[2]; } __attribute__((may_alias));
  struct FmB4 { uint8_t x[4]; } __attribute__((may_alias));
  struct FmB8 { uint8_t x[8]; } __attribute__((may_alias));
#define NWII_FM_TAB(K) ".pushsection nwii_fm_tab,\"a\"\n.balign 4\n.long 1b - .\n.long (%c[id] << 8) | " #K "\n.popsection\n"
#define NWII_FM_LD(ins, MT, K)                                                                  \
  uint64_t v; const uint64_t i = addr;                                                          \
  __asm__ volatile("1: " ins "\n" NWII_FM_TAB(K)                                                \
                   : [v] "=r"(v)                                                                \
                   : [b] "r"(fb), [i] "r"(i), [id] "i"(ID), "m"(*(const MT *)(fb + i)));
#define NWII_FM_ST(ins, MT, K, VAL)                                                             \
  const uint64_t i = addr; const uint64_t sv = (VAL);                                           \
  __asm__ volatile("1: " ins "\n" NWII_FM_TAB(K)                                                \
                   : "=m"(*(MT *)(fb + i))                                                      \
                   : [v] "r"(sv), [b] "r"(fb), [i] "r"(i), [id] "i"(ID));
  template <uint32_t ID> NWII_FORCEINLINE uint8_t read8(uint32_t addr) {
    NWII_FM_LD("movzbl (%[b],%[i]), %k[v]", FmB1, 1) return uint8_t(v); }
  template <uint32_t ID> NWII_FORCEINLINE uint16_t read16(uint32_t addr) {
    NWII_FM_LD("movzwl (%[b],%[i]), %k[v]", FmB2, 2) return __builtin_bswap16(uint16_t(v)); }
  template <uint32_t ID> NWII_FORCEINLINE uint32_t read32(uint32_t addr) {
    NWII_FM_LD("movl (%[b],%[i]), %k[v]", FmB4, 3) return __builtin_bswap32(uint32_t(v)); }
  template <uint32_t ID> NWII_FORCEINLINE uint64_t read64(uint32_t addr) {
    NWII_FM_LD("movq (%[b],%[i]), %q[v]", FmB8, 4) return __builtin_bswap64(v); }
  template <uint32_t ID> NWII_FORCEINLINE float read_f32(uint32_t addr) {
    uint32_t val = read32<ID>(addr); float f; std::memcpy(&f, &val, 4); return f; }
  template <uint32_t ID> NWII_FORCEINLINE double read_f64(uint32_t addr) {
    NWII_FM_LD("movq (%[b],%[i]), %q[v]", FmB8, 6) v = __builtin_bswap64(v); double f; std::memcpy(&f, &v, 8); return f; }
  template <uint32_t ID> NWII_FORCEINLINE void write8(uint32_t addr, uint8_t value) {
    NWII_FM_ST("movb %b[v], (%[b],%[i])", FmB1, 7, value) }
  template <uint32_t ID> NWII_FORCEINLINE void write16(uint32_t addr, uint16_t value) {
    NWII_FM_ST("movw %w[v], (%[b],%[i])", FmB2, 8, __builtin_bswap16(value)) }
  template <uint32_t ID> NWII_FORCEINLINE void write32(uint32_t addr, uint32_t value) {
    NWII_FM_ST("movl %k[v], (%[b],%[i])", FmB4, 9, __builtin_bswap32(value)) }
  template <uint32_t ID> NWII_FORCEINLINE void write64(uint32_t addr, uint64_t value) {
    NWII_FM_ST("movq %q[v], (%[b],%[i])", FmB8, 10, __builtin_bswap64(value)) }
  template <uint32_t ID> NWII_FORCEINLINE void write_f32(uint32_t addr, float value) {
    uint32_t bits; std::memcpy(&bits, &value, 4);
    NWII_FM_ST("movl %k[v], (%[b],%[i])", FmB4, 11, __builtin_bswap32(bits)) }
  template <uint32_t ID> NWII_FORCEINLINE void write_f64(uint32_t addr, double value) {
    uint64_t bits; std::memcpy(&bits, &value, 8);
    NWII_FM_ST("movq %q[v], (%[b],%[i])", FmB8, 12, __builtin_bswap64(bits)) }
#undef NWII_FM_LD
#undef NWII_FM_ST
#undef NWII_FM_TAB
#else
  template <uint32_t ID> NWII_FORCEINLINE uint8_t read8(uint32_t a) { return read8_ref(a); }
  template <uint32_t ID> NWII_FORCEINLINE uint16_t read16(uint32_t a) { return read16_ref(a); }
  template <uint32_t ID> NWII_FORCEINLINE uint32_t read32(uint32_t a) { return read32_ref(a); }
  template <uint32_t ID> NWII_FORCEINLINE uint64_t read64(uint32_t a) { return read64_ref(a); }
  template <uint32_t ID> NWII_FORCEINLINE float read_f32(uint32_t a) { return read_f32_ref(a); }
  template <uint32_t ID> NWII_FORCEINLINE double read_f64(uint32_t a) { return read_f64_ref(a); }
  template <uint32_t ID> NWII_FORCEINLINE void write8(uint32_t a, uint8_t v) { write8_ref(a, v); }
  template <uint32_t ID> NWII_FORCEINLINE void write16(uint32_t a, uint16_t v) { write16_ref(a, v); }
  template <uint32_t ID> NWII_FORCEINLINE void write32(uint32_t a, uint32_t v) { write32_ref(a, v); }
  template <uint32_t ID> NWII_FORCEINLINE void write64(uint32_t a, uint64_t v) { write64_ref(a, v); }
  template <uint32_t ID> NWII_FORCEINLINE void write_f32(uint32_t a, float v) { write_f32_ref(a, v); }
  template <uint32_t ID> NWII_FORCEINLINE void write_f64(uint32_t a, double v) { write_f64_ref(a, v); }
#endif
};
'''
body = body[:-len('};\n')] + tmpl
src = src[:i0] + body + src[i1:]

# 3. psq fast paths: templated; fastmem flavour uses the site accessors for float GQRs
rep('''NWII_FORCEINLINE void psq_l_fast(CPUContext &ctx, MemView &mv, uint32_t D, uint32_t ea, uint32_t W,
                                 uint32_t I) {''', '''#if NWII_FASTMEM && defined(__x86_64__)
template <uint32_t ID>
NWII_FORCEINLINE void psq_l_fast(CPUContext &ctx, MemView &mv, uint32_t D, uint32_t ea, uint32_t W, uint32_t I) {
  if (NWII_LIKELY((ctx.gqr[I] & 0x70000u) == 0)) {
    const double p0 = mv.read_f32<ID>(ea);
    const double p1 = W == 0 ? (double)mv.read_f32<ID>(ea + 4) : 1.0;
    ctx.fpr[D] = p0;
    ctx.ps1[D] = p1;
    return;
  }
  ctx.psq_load(D, ea, W, I);
}
template <uint32_t ID>
NWII_FORCEINLINE void psq_st_fast(CPUContext &ctx, MemView &mv, uint32_t S, uint32_t ea, uint32_t W, uint32_t I) {
  if (NWII_LIKELY((ctx.gqr[I] & 0x7u) == 0)) {
    const double p0 = ctx.fpr[S], p1 = ctx.ps1[S];
    mv.write_f32<ID>(ea, f32_from(ppc_single_bits_ftz(p0)));
    if (W == 0) mv.write_f32<ID>(ea + 4, f32_from(ppc_single_bits_ftz(p1)));
    return;
  }
  ctx.psq_store(S, ea, W, I);
}
#else
template <uint32_t ID>
NWII_FORCEINLINE void psq_l_fast(CPUContext &ctx, MemView &mv, uint32_t D, uint32_t ea, uint32_t W,
                                 uint32_t I) {''')
rep('''NWII_FORCEINLINE void psq_st_fast(CPUContext &ctx, MemView &mv, uint32_t S, uint32_t ea, uint32_t W,
                                  uint32_t I) {''', '''template <uint32_t ID>
NWII_FORCEINLINE void psq_st_fast(CPUContext &ctx, MemView &mv, uint32_t S, uint32_t ea, uint32_t W,
                                  uint32_t I) {''')
_k = src.rindex('''  ctx.psq_store(S, ea, W, I);
}
''') + len('''  ctx.psq_store(S, ea, W, I);
}
''')
src = src[:_k] + ('''  ctx.psq_store(S, ea, W, I);
}
#endif
// listed MMIO psq_st sites (gather pipe): straight to the out-of-line path
NWII_FORCEINLINE void psq_st_io(CPUContext &ctx, MemView &, uint32_t S, uint32_t ea, uint32_t W, uint32_t I) {
  ctx.psq_store(S, ea, W, I);
}
NWII_FORCEINLINE void psq_l_io(CPUContext &ctx, MemView &, uint32_t D, uint32_t ea, uint32_t W, uint32_t I) {
  ctx.psq_load(D, ea, W, I);
}
''') + src[_k:]
open(P, 'w').write(src)
print('ok')
