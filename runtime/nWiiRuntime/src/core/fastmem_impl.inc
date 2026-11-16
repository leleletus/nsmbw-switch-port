// R12 fastmem runtime side (included from src/hle/memory.cpp so no build file changes).
// See r9/r12fm/LEEME.md and r9/r11c/DISENO_FASTMEM.md.
//  - fm_alloc(): canonical blocks for MMU::mem1/mem2/lc. With NWII_FASTMEM (PC, Linux x86-64):
//    one memfd per region, mapped once for the runtime (canonical, + 1 zero page of padding) and
//    again into a 4 GiB + 64 KiB PROT_NONE reservation at g_fm_base + ea (MEM1 x4, MEM2 x4, LC).
//  - SIGSEGV handler: fault inside the window on an instruction listed in nwii_fm_tab ->
//    decode (x86-64 mov/movzx with ModRM/SIB), MMU::*_slow by the site's kind, write the
//    destination register, skip the instruction. Per-site fault counters, dumped at exit /
//    SIGTERM / SIGINT and every NWII_FASTMEM_DUMP_S seconds (default 30) to stdout:
//    "[FASTMEM] site <id> kind <k> faults <n> last_ea <ea>" (site id -> guest pc: r9/r12fm/sites.tsv).
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <thread>
#include <vector>
#if NWII_FASTMEM && defined(__x86_64__) && defined(__linux__)
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif


#if NWII_FASTMEM && defined(__SWITCH__) && defined(__aarch64__)
// ---- Switch (libnx, Atmosphere): r9/r11c/proto/fastmem_nx.cpp, tested in tools/fmtest ----
#include <switch.h>
#include "a64_decode.h"
extern "C" const char __start_nwii_fm_tab[] __attribute__((weak));
extern "C" const char __stop_nwii_fm_tab[] __attribute__((weak));
namespace nwii {
namespace runtime {
extern MMU *g_mmu;
namespace fmnx {
constexpr uint64_t kSpan = 1ull << 32, kGuard = 0x10000;
constexpr uint64_t kOffMem1 = 0, kOffMem2 = 32ull << 20, kOffLC = (96ull << 20) + 0x10000, kTotal = 98ull << 20;
struct TabEntry { int32_t rel; uint32_t info; };
struct Site { uint64_t pc; uint32_t info; };
Site *g_sites;
size_t g_nsites;
uint32_t g_maxid;
std::atomic<uint32_t> *g_site_faults;
uint32_t *g_site_last_ea;
std::atomic<uint64_t> g_faults{0};
uint8_t *g_canon;

const Site *find_site(uint64_t pc) {
  const Site *lo = g_sites, *hi = g_sites + g_nsites;
  while (lo < hi) {
    const Site *mid = lo + (hi - lo) / 2;
    if (mid->pc < pc) lo = mid + 1; else hi = mid;
  }
  return (lo != g_sites + g_nsites && lo->pc == pc) ? lo : nullptr;
}

bool setup() {
  Handle self = envGetOwnProcessHandle();
  if (self == INVALID_HANDLE) return false;
  void *src = aligned_alloc(0x200000, kTotal);
  if (!src) return false;
  std::memset(src, 0, kTotal);
  virtmemLock();
  void *code = virtmemFindCodeMemory(kTotal, 0x1000);
  Result rc = svcMapProcessCodeMemory(self, (u64)code, (u64)src, kTotal);
  if (R_SUCCEEDED(rc)) rc = svcSetProcessMemoryPermission(self, (u64)code, kTotal, Perm_Rw);
  uint8_t *hole = nullptr;
  if (R_SUCCEEDED(rc)) {
    hole = (uint8_t *)virtmemFindAslr(kSpan + kGuard + 0x200000, 0x1000);
    hole = (uint8_t *)(((uintptr_t)hole + 0x1FFFFF) & ~(uintptr_t)0x1FFFFF);
    if (!virtmemAddReservation(hole, kSpan + kGuard)) rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
  }
  virtmemUnlock();
  if (R_FAILED(rc)) { std::printf("[FASTMEM] mapping failed rc=0x%x\n", rc); return false; }
  g_canon = (uint8_t *)code;
  for (uint32_t m : {0x00000000u, 0x40000000u, 0x80000000u, 0xC0000000u}) {
    if (R_FAILED(svcMapProcessMemory(hole + m, self, (u64)code + kOffMem1, 24u << 20))) return false;
    if (R_FAILED(svcMapProcessMemory(hole + m + 0x10000000u, self, (u64)code + kOffMem2, 64u << 20))) return false;
  }
  if (R_FAILED(svcMapProcessMemory(hole + MMU::kLCBase, self, (u64)code + kOffLC, MMU::kLCSize))) return false;
  const TabEntry *t0 = reinterpret_cast<const TabEntry *>(__start_nwii_fm_tab);
  const TabEntry *t1 = reinterpret_cast<const TabEntry *>(__stop_nwii_fm_tab);
  g_nsites = t0 ? size_t(t1 - t0) : 0;
  g_sites = (Site *)std::malloc(sizeof(Site) * (g_nsites ? g_nsites : 1));
  for (size_t i = 0; i < g_nsites; ++i) {
    g_sites[i] = {uint64_t((intptr_t)&t0[i] + t0[i].rel), t0[i].info};
    g_maxid = std::max(g_maxid, t0[i].info >> 8);
  }
  std::sort(g_sites, g_sites + g_nsites, [](const Site &a, const Site &b) { return a.pc < b.pc; });
  g_site_faults = new std::atomic<uint32_t>[g_maxid + 1]();
  g_site_last_ea = new uint32_t[g_maxid + 1]();
  g_fm_base = hole;  // last: the exception entry ignores faults until the window exists
  std::printf("[FASTMEM] on (Switch): window %p, canonical %p, %zu access sites\n", (void *)hole, code, g_nsites);
  return true;
}

void dump(const char *why) {
  if (!g_site_faults) return;
  uint32_t nsites = 0;
  for (uint32_t i = 0; i <= g_maxid; ++i) nsites += g_site_faults[i].load() != 0;
  std::printf("[FASTMEM] dump (%s): %llu faults, %u sites faulted\n", why, (unsigned long long)g_faults.load(), nsites);
  for (uint32_t i = 0; i <= g_maxid; ++i)
    if (uint32_t n = g_site_faults[i].load()) std::printf("[FASTMEM] site %u faults %u last_ea %08X\n", i, n, g_site_last_ea[i]);
}
}  // namespace fmnx
}  // namespace runtime
}  // namespace nwii

struct alignas(16) FmExcArea {
  uint64_t x[32];
  uint64_t q[64];
  uint64_t fpsr, fpcr, ksp, pad;
};
extern "C" {
FmExcArea g_fm_exc_area;
// r34s2: el area y la pila de excepcion son unicas: con dos hilos que ejecutan codigo generado (juego y
// CPU de sonido) dos fallos a la vez se pisarian. Candado de giro tomado en la entrada (solo x6-x8).
volatile uint32_t g_fm_exc_lock = 0;
alignas(16) uint8_t g_fm_exc_stack[256 * 1024];
uint8_t *g_fm_exc_stack_top = g_fm_exc_stack + sizeof g_fm_exc_stack;

uint32_t fm_exception_c(uint64_t type, ThreadExceptionFrameA64 *f, FmExcArea *area) {
  using namespace nwii::runtime;
  using namespace nwii::runtime::fmnx;
  const uint32_t ec = f->esr >> 26;
  if (type != ThreadExceptionDesc_Other || (ec != 0x24 && ec != 0x25)) return 1;
  if (!g_fm_base || !g_mmu || f->far - (uint64_t)g_fm_base >= kSpan + kGuard) return 1;
  const Site *s = find_site(f->elr_el1);
  if (!s) return 1;
  const A64Access a = a64_decode_ldst(*(const uint32_t *)f->elr_el1);
  if (!a.ok || a.fp || a.mode != A64Access::RegOffset) return 1;
  auto getx = [&](int n) -> uint64_t {
    if (n == 31) return 0;
    if (n <= 8) return f->cpu_gprs[n];
    if (n == 30) return f->lr;
    return area->x[n];
  };
  auto setx = [&](int n, uint64_t v) {
    if (n <= 8) f->cpu_gprs[n] = v;
    else if (n == 30) f->lr = v;
    else if (n < 31) area->x[n] = v;
  };
  const uint64_t host = getx(a.rn) + (uint64_t(uint32_t(getx(a.rm))) << a.shift);
  const uint32_t ea = uint32_t(host - (uint64_t)g_fm_base);
  const uint32_t kind = s->info & 0xFF, id = s->info >> 8;
  g_faults.fetch_add(1, std::memory_order_relaxed);
  if (id <= g_maxid) { g_site_faults[id].fetch_add(1, std::memory_order_relaxed); g_site_last_ea[id] = ea; }
  MMU &m = *g_mmu;
  auto sw = [](uint64_t v, int n) -> uint64_t {
    return n == 1 ? (v & 0xFF) : n == 2 ? __builtin_bswap16(uint16_t(v)) : n == 4 ? __builtin_bswap32(uint32_t(v)) : __builtin_bswap64(v);
  };
  if (a.store) {
    const uint64_t lv = sw(getx(a.rt), a.size);
    switch (kind) {
      case FM_W8: m.write8_slow(ea, uint8_t(lv)); break;
      case FM_W16: m.write16_slow(ea, uint16_t(lv)); break;
      case FM_W32: m.write32_slow(ea, uint32_t(lv)); break;
      case FM_W64: m.write64_slow(ea, lv); break;
      case FM_WF32: { uint32_t b = uint32_t(lv); float x; std::memcpy(&x, &b, 4); m.write_f32_slow(ea, x); break; }
      case FM_WF64: { double x; std::memcpy(&x, &lv, 8); m.write_f64_slow(ea, x); break; }
      default: return 1;
    }
  } else {
    uint64_t lv;
    switch (kind) {
      case FM_R8: lv = m.read8_slow(ea); break;
      case FM_R16: lv = m.read16_slow(ea); break;
      case FM_R32: case FM_RF32: lv = m.read32_slow(ea); break;
      case FM_R64: lv = m.read64_slow(ea); break;
      case FM_RF64: { const double x = m.read_f64_slow(ea); std::memcpy(&lv, &x, 8); break; }
      default: return 1;
    }
    setx(a.rt, sw(lv, a.size));
  }
  f->elr_el1 += 4;
  return 0;
}
}

__asm__(R"(
  .section .text.__libnx_exception_entry, "ax", %progbits
  .global __libnx_exception_entry
  .type __libnx_exception_entry, %function
  .balign 4
__libnx_exception_entry:
  adrp x7, g_fm_exc_lock
  add  x7, x7, :lo12:g_fm_exc_lock
1:
  ldaxr w8, [x7]
  cbnz w8, 1b
  mov  w8, #1
  stlxr w6, w8, [x7]
  cbnz w6, 1b
  adrp x2, g_fm_exc_area
  add  x2, x2, :lo12:g_fm_exc_area
  stp  x9,  x10, [x2, #8*9]
  stp  x11, x12, [x2, #8*11]
  stp  x13, x14, [x2, #8*13]
  stp  x15, x16, [x2, #8*15]
  stp  x17, x18, [x2, #8*17]
  stp  x19, x20, [x2, #8*19]
  stp  x21, x22, [x2, #8*21]
  stp  x23, x24, [x2, #8*23]
  stp  x25, x26, [x2, #8*25]
  stp  x27, x28, [x2, #8*27]
  str  x29,      [x2, #8*29]
  add  x3, x2, #256
  st1  {v0.2d,  v1.2d,  v2.2d,  v3.2d},  [x3], #64
  st1  {v4.2d,  v5.2d,  v6.2d,  v7.2d},  [x3], #64
  st1  {v8.2d,  v9.2d,  v10.2d, v11.2d}, [x3], #64
  st1  {v12.2d, v13.2d, v14.2d, v15.2d}, [x3], #64
  st1  {v16.2d, v17.2d, v18.2d, v19.2d}, [x3], #64
  st1  {v20.2d, v21.2d, v22.2d, v23.2d}, [x3], #64
  st1  {v24.2d, v25.2d, v26.2d, v27.2d}, [x3], #64
  st1  {v28.2d, v29.2d, v30.2d, v31.2d}, [x3], #64
  mrs  x4, fpsr
  mrs  x5, fpcr
  mov  x6, sp
  stp  x4, x5, [x3]
  str  x6, [x3, #16]
  adrp x4, g_fm_exc_stack_top
  ldr  x4, [x4, :lo12:g_fm_exc_stack_top]
  mov  sp, x4
  bl   fm_exception_c
  adrp x2, g_fm_exc_area
  add  x2, x2, :lo12:g_fm_exc_area
  add  x3, x2, #256
  ld1  {v0.2d,  v1.2d,  v2.2d,  v3.2d},  [x3], #64
  ld1  {v4.2d,  v5.2d,  v6.2d,  v7.2d},  [x3], #64
  ld1  {v8.2d,  v9.2d,  v10.2d, v11.2d}, [x3], #64
  ld1  {v12.2d, v13.2d, v14.2d, v15.2d}, [x3], #64
  ld1  {v16.2d, v17.2d, v18.2d, v19.2d}, [x3], #64
  ld1  {v20.2d, v21.2d, v22.2d, v23.2d}, [x3], #64
  ld1  {v24.2d, v25.2d, v26.2d, v27.2d}, [x3], #64
  ld1  {v28.2d, v29.2d, v30.2d, v31.2d}, [x3], #64
  ldp  x4, x5, [x3]
  ldr  x6, [x3, #16]
  msr  fpsr, x4
  msr  fpcr, x5
  mov  sp, x6
  ldp  x9,  x10, [x2, #8*9]
  ldp  x11, x12, [x2, #8*11]
  ldp  x13, x14, [x2, #8*13]
  ldp  x15, x16, [x2, #8*15]
  ldp  x17, x18, [x2, #8*17]
  ldp  x19, x20, [x2, #8*19]
  ldp  x21, x22, [x2, #8*21]
  ldp  x23, x24, [x2, #8*23]
  ldp  x25, x26, [x2, #8*25]
  ldp  x27, x28, [x2, #8*27]
  ldr  x29,      [x2, #8*29]
  adrp x7, g_fm_exc_lock
  add  x7, x7, :lo12:g_fm_exc_lock
  stlr wzr, [x7]
  svc  0x28
  .size __libnx_exception_entry, . - __libnx_exception_entry
  .text
)");
#endif
#if NWII_FASTMEM && defined(__x86_64__) && defined(__linux__)
extern "C" const char __start_nwii_fm_tab[] __attribute__((weak));
extern "C" const char __stop_nwii_fm_tab[] __attribute__((weak));
#endif
namespace nwii {
namespace runtime {
extern MMU *g_mmu;
uint8_t *g_fm_base = nullptr;

namespace {
constexpr size_t kM1 = 24u << 20, kM2 = 64u << 20, kLC = MMU::kLCSize;

void plain_alloc(MemBlock &b, size_t n) {
  b.p = static_cast<uint8_t *>(std::calloc(n + 8, 1));
  b.n = n + 8;
  if (!b.p) { std::fprintf(stderr, "[FASTMEM] out of memory\n"); std::abort(); }
}
}  // namespace

#if NWII_FASTMEM && defined(__x86_64__) && defined(__linux__)
namespace {
constexpr uint64_t kSpan = 1ull << 32, kGuard = 0x10000;

struct TabEntry { int32_t rel; uint32_t info; };
struct Site { uint64_t pc; uint32_t info; };
Site *g_sites = nullptr;
size_t g_nsites = 0;
uint32_t g_maxid = 0;
std::atomic<uint32_t> *g_site_faults = nullptr;
uint32_t *g_site_last_ea = nullptr;
std::atomic<uint64_t> g_faults{0};
std::atomic<uint64_t> g_unhandled{0};
struct sigaction g_old_segv;
struct sigaction g_old_term, g_old_int;

const Site *find_site(uint64_t pc) {
  const Site *lo = g_sites, *hi = g_sites + g_nsites;
  while (lo < hi) {
    const Site *mid = lo + (hi - lo) / 2;
    if (mid->pc < pc) lo = mid + 1; else hi = mid;
  }
  return (lo != g_sites + g_nsites && lo->pc == pc) ? lo : nullptr;
}

struct X86Access {
  bool store = false;
  int size = 0;
  int reg = -1;
  bool high8 = false;
  bool zext = false;
  uint64_t imm = 0;
  uint64_t ea = 0;
  int len = 0;
};
const int kGreg[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                       REG_R8,  REG_R9,  REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};

// From r9/r11c/proto/fastmem_proto.cpp (tested there).
bool x86_decode(const uint8_t *ip, const greg_t *g, X86Access &a) {
  const uint8_t *p = ip;
  bool opsize = false;
  uint8_t rex = 0;
  for (;;) {
    if (*p == 0x66) { opsize = true; ++p; continue; }
    if (*p == 0x2E || *p == 0x3E) { ++p; continue; }
    break;
  }
  if ((*p & 0xF0) == 0x40) rex = *p++;
  const bool W = rex & 8, R = rex & 4, X = rex & 2, Bx = rex & 1;
  uint8_t op = *p++;
  bool twobyte = false;
  if (op == 0x0F) { twobyte = true; op = *p++; }
  if (!twobyte) {
    switch (op) {
      case 0x8A: a.store = false; a.size = 1; break;
      case 0x8B: a.store = false; a.size = W ? 8 : opsize ? 2 : 4; break;
      case 0x88: a.store = true; a.size = 1; break;
      case 0x89: a.store = true; a.size = W ? 8 : opsize ? 2 : 4; break;
      default: return false;
    }
  } else {
    if (op == 0xB6) { a.size = 1; a.zext = true; }
    else if (op == 0xB7) { a.size = 2; a.zext = true; }
    else return false;
  }
  const uint8_t modrm = *p++;
  const int mod = modrm >> 6, regf = ((modrm >> 3) & 7) | (R ? 8 : 0);
  int rm = modrm & 7;
  if (mod == 3) return false;
  uint64_t ea = 0;
  if (rm == 4) {
    const uint8_t sib = *p++;
    const int scale = sib >> 6, idx = ((sib >> 3) & 7) | (X ? 8 : 0), base = (sib & 7) | (Bx ? 8 : 0);
    if (idx != 4) ea += uint64_t(g[kGreg[idx]]) << scale;
    if ((sib & 7) == 5 && mod == 0) { int32_t d; std::memcpy(&d, p, 4); p += 4; ea += int64_t(d); }
    else ea += uint64_t(g[kGreg[base]]);
  } else if (rm == 5 && mod == 0) {
    return false;
  } else {
    ea += uint64_t(g[kGreg[rm | (Bx ? 8 : 0)]]);
  }
  if (mod == 1) { ea += int64_t(int8_t(*p)); p += 1; }
  else if (mod == 2) { int32_t d; std::memcpy(&d, p, 4); p += 4; ea += int64_t(d); }
  a.reg = regf;
  if (a.size == 1 && !rex && regf >= 4 && regf < 8 && !a.zext) { a.high8 = true; a.reg = regf - 4; }
  a.ea = ea;
  a.len = int(p - ip);
  return true;
}

inline uint64_t swap_n(uint64_t v, int n) {
  switch (n) {
    case 1: return v & 0xFF;
    case 2: return __builtin_bswap16(uint16_t(v));
    case 4: return __builtin_bswap32(uint32_t(v));
    default: return __builtin_bswap64(v);
  }
}

void chain(int sig, siginfo_t *si, void *uc) {
  g_unhandled.fetch_add(1, std::memory_order_relaxed);
  if ((g_old_segv.sa_flags & SA_SIGINFO) && g_old_segv.sa_sigaction) {
    g_old_segv.sa_sigaction(sig, si, uc);
    return;
  }
  if (g_old_segv.sa_handler != SIG_DFL && g_old_segv.sa_handler != SIG_IGN && g_old_segv.sa_handler) {
    g_old_segv.sa_handler(sig);
    return;
  }
  signal(sig, SIG_DFL);  // a real crash: the instruction faults again and the process dies
}

void on_segv(int sig, siginfo_t *si, void *ucv) {
  auto *uc = static_cast<ucontext_t *>(ucv);
  greg_t *g = uc->uc_mcontext.gregs;
  const uint64_t fa = reinterpret_cast<uint64_t>(si->si_addr);
  const uint64_t B = reinterpret_cast<uint64_t>(g_fm_base);
  if (!g_mmu || fa - B >= kSpan + kGuard) return chain(sig, si, ucv);
  const uint64_t pc = uint64_t(g[REG_RIP]);
  const Site *s = find_site(pc);
  X86Access a;
  if (!s || !x86_decode(reinterpret_cast<const uint8_t *>(pc), g, a) || a.ea - B >= kSpan + kGuard) {
    std::fprintf(stderr, "[FASTMEM] unhandled fault rip=%p addr=%p (site %s)\n", (void *)pc, si->si_addr,
                 s ? "listed, decode failed" : "not listed");
    return chain(sig, si, ucv);
  }
  const uint32_t ea = uint32_t(a.ea - B);
  const uint32_t kind = s->info & 0xFF, id = s->info >> 8;
  g_faults.fetch_add(1, std::memory_order_relaxed);
  if (id <= g_maxid) {
    g_site_faults[id].fetch_add(1, std::memory_order_relaxed);
    g_site_last_ea[id] = ea;
  }
  MMU &m = *g_mmu;
  if (a.store) {
    uint64_t v = uint64_t(g[kGreg[a.reg]]);
    if (a.high8) v >>= 8;
    const uint64_t lv = swap_n(v, a.size);  // memory-order register image -> guest value
    switch (kind) {
      case FM_W8: m.write8_slow(ea, uint8_t(lv)); break;
      case FM_W16: m.write16_slow(ea, uint16_t(lv)); break;
      case FM_W32: m.write32_slow(ea, uint32_t(lv)); break;
      case FM_W64: m.write64_slow(ea, lv); break;
      case FM_WF32: { uint32_t b = uint32_t(lv); float f; std::memcpy(&f, &b, 4); m.write_f32_slow(ea, f); break; }
      case FM_WF64: { double d; std::memcpy(&d, &lv, 8); m.write_f64_slow(ea, d); break; }
      default: return chain(sig, si, ucv);
    }
  } else {
    uint64_t lv;
    switch (kind) {
      case FM_R8: lv = m.read8_slow(ea); break;
      case FM_R16: lv = m.read16_slow(ea); break;
      case FM_R32: case FM_RF32: lv = m.read32_slow(ea); break;
      case FM_R64: lv = m.read64_slow(ea); break;
      case FM_RF64: { const double d = m.read_f64_slow(ea); std::memcpy(&lv, &d, 8); break; }
      default: return chain(sig, si, ucv);
    }
    const uint64_t v = swap_n(lv, a.size);
    greg_t &r = g[kGreg[a.reg]];
    if (a.zext || a.size >= 4) r = greg_t(v);
    else if (a.size == 2) r = greg_t((uint64_t(r) & ~0xFFFFull) | v);
    else if (a.high8) r = greg_t((uint64_t(r) & ~0xFF00ull) | (v << 8));
    else r = greg_t((uint64_t(r) & ~0xFFull) | v);
  }
  g[REG_RIP] += a.len;
}

void fm_dump(const char *why) {
  if (!g_site_faults) return;
  struct Row { uint32_t id, n; };
  std::vector<Row> rows;
  for (uint32_t i = 0; i <= g_maxid; ++i) {
    const uint32_t n = g_site_faults[i].load(std::memory_order_relaxed);
    if (n) rows.push_back({i, n});
  }
  std::sort(rows.begin(), rows.end(), [](const Row &x, const Row &y) { return x.n > y.n; });
  std::printf("[FASTMEM] dump (%s): %llu faults, %zu sites faulted, %llu unhandled\n", why,
              (unsigned long long)g_faults.load(), rows.size(), (unsigned long long)g_unhandled.load());
  for (const Row &r : rows) {
    uint32_t kind = 0;
    for (size_t i = 0; i < g_nsites; ++i)
      if ((g_sites[i].info >> 8) == r.id) { kind = g_sites[i].info & 0xFF; break; }
    std::printf("[FASTMEM] site %u kind %u faults %u last_ea %08X\n", r.id, kind, r.n, g_site_last_ea[r.id]);
  }
  std::fflush(stdout);
}

void on_term(int sig) {
  fm_dump(sig == SIGTERM ? "SIGTERM" : "SIGINT");
  struct sigaction &o = sig == SIGTERM ? g_old_term : g_old_int;
  sigaction(sig, &o, nullptr);
  raise(sig);
}

void dump_thread() {
  const char *e = std::getenv("NWII_FASTMEM_DUMP_S");
  const int s = e ? std::atoi(e) : 30;
  if (s <= 0) return;
  std::thread([s] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(s));
      fm_dump("periodic");
    }
  }).detach();
}

bool fm_setup(MemBlock &m1, MemBlock &m2, MemBlock &lc) {
  uint8_t *B = static_cast<uint8_t *>(
      mmap(nullptr, kSpan + kGuard, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
  if (B == MAP_FAILED) { std::perror("[FASTMEM] reserve"); return false; }
  auto mk = [&](const char *name, size_t size, MemBlock &canon, std::initializer_list<uint32_t> at) -> bool {
    const int fd = memfd_create(name, MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size + 4096) != 0) { std::perror("[FASTMEM] memfd"); return false; }
    void *c = mmap(nullptr, size + 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (c == MAP_FAILED) { std::perror("[FASTMEM] canonical"); close(fd); return false; }
    canon.p = static_cast<uint8_t *>(c);
    canon.n = size + 8;
    for (uint32_t off : at)
      if (mmap(B + off, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED) {
        std::perror("[FASTMEM] view");
        close(fd);
        return false;
      }
    close(fd);
    return true;
  };
  if (!mk("nwii_mem1", kM1, m1, {0x00000000u, 0x40000000u, 0x80000000u, 0xC0000000u}) ||
      !mk("nwii_mem2", kM2, m2, {0x10000000u, 0x50000000u, 0x90000000u, 0xD0000000u}) ||
      !mk("nwii_lc", kLC, lc, {MMU::kLCBase}))
    return false;
  g_fm_base = B;
  const TabEntry *tab0 = reinterpret_cast<const TabEntry *>(__start_nwii_fm_tab);
  const TabEntry *tab1 = reinterpret_cast<const TabEntry *>(__stop_nwii_fm_tab);
  g_nsites = tab0 ? size_t(tab1 - tab0) : 0;
  g_sites = static_cast<Site *>(std::malloc(sizeof(Site) * (g_nsites ? g_nsites : 1)));
  for (size_t i = 0; i < g_nsites; ++i) {
    const TabEntry *e = &tab0[i];
    g_sites[i] = {uint64_t(intptr_t(e) + e->rel), e->info};
    g_maxid = std::max(g_maxid, e->info >> 8);
  }
  std::sort(g_sites, g_sites + g_nsites, [](const Site &x, const Site &y) { return x.pc < y.pc; });
  g_site_faults = new std::atomic<uint32_t>[g_maxid + 1]();
  g_site_last_ea = new uint32_t[g_maxid + 1]();
  struct sigaction sa {};
  sa.sa_sigaction = on_segv;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, &g_old_segv);
  struct sigaction st {};
  st.sa_handler = on_term;
  sigemptyset(&st.sa_mask);
  sigaction(SIGTERM, &st, &g_old_term);
  sigaction(SIGINT, &st, &g_old_int);
  std::atexit([] { fm_dump("exit"); });
  dump_thread();
  std::printf("[FASTMEM] on: window %p, %zu access sites (max id %u)\n", (void *)B, g_nsites, g_maxid);
  return true;
}
}  // namespace
#endif

// r34s2: un segundo CPUContext (CPU de sonido) comparte los bloques del principal: no se reserva nada.
bool g_fm_alias_next = false;
void fm_alloc(MemBlock &mem1, MemBlock &mem2, MemBlock &lc) {
  if (g_fm_alias_next) return;
#if NWII_FASTMEM && defined(__x86_64__) && defined(__linux__)
  static bool done = false;
  if (!done) {
    done = true;
    if (fm_setup(mem1, mem2, lc)) return;
    std::fprintf(stderr, "[FASTMEM] setup failed: the fastmem build cannot run without it\n");
    std::abort();
  }
#endif
#if NWII_FASTMEM && defined(__SWITCH__) && defined(__aarch64__)
  static bool done_nx = false;
  if (!done_nx) {
    done_nx = true;
    if (fmnx::setup()) {
      mem1.p = fmnx::g_canon + fmnx::kOffMem1; mem1.n = kM1 + 8;
      mem2.p = fmnx::g_canon + fmnx::kOffMem2; mem2.n = kM2 + 8;
      lc.p = fmnx::g_canon + fmnx::kOffLC; lc.n = kLC + 8;
      std::atexit([] { fmnx::dump("exit"); });
      return;
    }
    std::printf("[FASTMEM] Switch setup failed: the fastmem build cannot run without it\n");
    std::abort();
  }
#endif
  plain_alloc(mem1, kM1);
  plain_alloc(mem2, kM2);
  plain_alloc(lc, kLC);
}

}  // namespace runtime
}  // namespace nwii
