#pragma once
#include <array>
#include <cmath>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include <mutex>
#include <atomic>
#include <queue>
#include <limits>
#include <stack>

#include "runtime/nwii_env.h"

// R6 perf: inlining control for the guest memory fast paths (MMU below).
#if defined(__GNUC__) || defined(__clang__)
#define NWII_FORCEINLINE inline __attribute__((always_inline))
#define NWII_NOINLINE __attribute__((noinline))
#define NWII_LIKELY(x) __builtin_expect(!!(x), 1)
#define NWII_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define NWII_FORCEINLINE inline
#define NWII_NOINLINE
#define NWII_LIKELY(x) (x)
#define NWII_UNLIKELY(x) (x)
#endif

namespace nwii {
namespace runtime {

struct ConditionField {
  bool lt; 
  bool gt; 
  bool eq; 
  bool so; 

  ConditionField() : lt(false), gt(false), eq(false), so(false) {}
};

void GX_WGPIPE_Write8(uint8_t val);
void GX_WGPIPE_Write16(uint16_t val);
void GX_WGPIPE_Write32(uint32_t val);
void GX_DL_Snapshot(uint32_t addr, uint32_t size);
bool GX_DL_Fetch(uint32_t addr, uint32_t size, std::vector<uint8_t>& out);
void GX_WGPIPE_WriteF32(float val);
void GX_WGPIPE_WriteF64(double val);

uint16_t HW_Reg_Read16(uint32_t addr);
uint32_t HW_Reg_Read32(uint32_t addr);
void HW_Reg_Write16(uint32_t addr, uint16_t val);
void HW_Reg_Write32(uint32_t addr, uint32_t val);

struct CPUContext;
void init_ipc_client(CPUContext &ctx);
bool handle_syscall(CPUContext &ctx);
// R9 guestloop: inline fast path of the interrupt poll (defined after
// CPUContext); the rest of the poll is in ios.cpp.
inline bool process_pending_callbacks(CPUContext &ctx);
bool process_pending_callbacks_slow(CPUContext &ctx);
// Kicks the poll countdown of the game CPU context (g_ctx_ptr), for code
// without a CPUContext at hand (ios.cpp).
void poll_kick_global();
namespace hw {
// Hidden: one static executable, so AArch64 code addresses them directly
// (adrp + ldr) instead of through the GOT (same in hw/hw.h).
extern std::atomic<uint32_t> pi_intsr __attribute__((visibility("hidden")));  // hw_pi.cpp
extern std::atomic<uint32_t> pi_intmr __attribute__((visibility("hidden")));
}

struct CallbackInfo {
    uint32_t cb_addr;
    uint32_t arg1;
    uint32_t arg2;
    bool is_irq;
};



void watch_hit(uint32_t addr, uint32_t value, int width);
// R9 cpucode: hidden (the NRO / PC binary is one statically linked module), so
// the generated code reads it PC-relative instead of through the GOT.
#if defined(__GNUC__)
#define NWII_HIDDEN __attribute__((visibility("hidden")))
#else
#define NWII_HIDDEN
#endif
extern uint32_t g_watch_addr NWII_HIDDEN;

// [r12fm-header] R12 fastmem (r9/r12fm/LEEME.md, r9/r11c/DISENO_FASTMEM.md).
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

// Strict Dolphin-accurate MMU
struct MMU {
  MemBlock mem1;
  MemBlock mem2;
  // Broadway locked L1 data cache ("LC"). LCEnable locks half of the L1 D-cache
  // and maps it at effective 0xE0000000 (DBAT3); nw4r g3d CalcWorld/CalcView
  // build every model's matrix array there and copy it out with the LC DMA
  // (SPR DMA_U/DMA_L, CPUContext::write_dmal). The hardware locks 16 KiB;
  // like Dolphin (Memory::L1_CACHE_SIZE, MMU.cpp ReadFromHardware) we back
  // 256 KiB so a stray access past 16 KiB behaves as in the reference.
  // Every guest load/store (incl. lfs/stfs, psq_l/psq_st, dcbz/dcbz_l and the
  // HLE helpers) goes through get_ptr, and the LC test sits on its miss path
  // (0xE0000000 masks to 0x20000000, neither MEM1 nor MEM2), so RAM accesses
  // pay nothing for it.
  static constexpr uint32_t kLCBase = 0xE0000000u;
  static constexpr uint32_t kLCSize = 0x40000u;
  MemBlock lc;

  MMU() { fm_alloc(mem1, mem2, lc); }
  MMU(const MMU &) = delete;
  MMU &operator=(const MMU &) = delete;

  inline uint8_t *get_ptr(uint32_t addr) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (paddr < mem1.size() - 8) {
        return &mem1[paddr];
    } else if (paddr >= 0x10000000 && paddr < 0x10000000 + mem2.size() - 8) {
        return &mem2[paddr - 0x10000000];
    } else if (addr - kLCBase < kLCSize) {
        return &lc[addr - kLCBase];
    }
    return nullptr;
  }

  // ---- R6 perf: inline fast paths ------------------------------------------
  // Generated code does every guest load/store through these. Out of line they
  // were ~28% of the CPU thread (perf, Switch-like CPU profile). The common
  // case (plain MEM1/MEM2 RAM, no NWII_WATCH watchpoint) is now a bounds check
  // plus one byte-swapped host access, inlined at the call site; everything
  // else (address 0xC, MMIO, the write-gather pipe, the locked cache, unmapped
  // addresses, the watchpoint) takes the original code, renamed *_slow.
  static constexpr uint32_t kMem1Size = 24u * 1024 * 1024;
  static constexpr uint32_t kMem2Size = 64u * 1024 * 1024;
  // Host pointer of a plain-RAM guest address, or nullptr. MEM1 starts at 0x10
  // so the 0x0000000C special case stays on the slow path. Accesses may run up
  // to 7 bytes past the end, as before (the vectors carry 8 bytes of padding).
  NWII_FORCEINLINE uint8_t *fast_ptr(uint32_t addr) {
    const uint32_t paddr = addr & 0x3FFFFFFF;
    if (NWII_LIKELY(paddr - 0x10u < kMem1Size - 0x10u)) return mem1.data() + paddr;
    if (paddr - 0x10000000u < kMem2Size) return mem2.data() + (paddr - 0x10000000u);
    return nullptr;
  }
  NWII_FORCEINLINE uint8_t read8(uint32_t addr) {
    if (uint8_t *p = fast_ptr(addr)) return *p;
    return read8_slow(addr);
  }
  NWII_FORCEINLINE uint16_t read16(uint32_t addr) {
    if (uint8_t *p = fast_ptr(addr)) { uint16_t v; std::memcpy(&v, p, 2); return __builtin_bswap16(v); }
    return read16_slow(addr);
  }
  NWII_FORCEINLINE uint32_t read32(uint32_t addr) {
    if (uint8_t *p = fast_ptr(addr)) { uint32_t v; std::memcpy(&v, p, 4); return __builtin_bswap32(v); }
    return read32_slow(addr);
  }
  NWII_FORCEINLINE uint64_t read64(uint32_t addr) {
    if (uint8_t *p = fast_ptr(addr)) { uint64_t v; std::memcpy(&v, p, 8); return __builtin_bswap64(v); }
    return read64_slow(addr);
  }
  NWII_FORCEINLINE double read_f64(uint32_t addr) {
    if (uint8_t *p = fast_ptr(addr)) {
      uint64_t v; std::memcpy(&v, p, 8); v = __builtin_bswap64(v);
      double f; std::memcpy(&f, &v, 8); return f;
    }
    return read_f64_slow(addr);
  }
  NWII_FORCEINLINE void write8(uint32_t addr, uint8_t value) {
    if (NWII_LIKELY(!g_watch_addr)) if (uint8_t *p = fast_ptr(addr)) { *p = value; return; }
    write8_slow(addr, value);
  }
  NWII_FORCEINLINE void write16(uint32_t addr, uint16_t value) {
    if (NWII_LIKELY(!g_watch_addr)) if (uint8_t *p = fast_ptr(addr)) {
      const uint16_t v = __builtin_bswap16(value); std::memcpy(p, &v, 2); return;
    }
    write16_slow(addr, value);
  }
  NWII_FORCEINLINE void write32(uint32_t addr, uint32_t value) {
    if (NWII_LIKELY(!g_watch_addr)) if (uint8_t *p = fast_ptr(addr)) {
      const uint32_t v = __builtin_bswap32(value); std::memcpy(p, &v, 4); return;
    }
    write32_slow(addr, value);
  }
  NWII_FORCEINLINE void write64(uint32_t addr, uint64_t value) {
    if (NWII_LIKELY(!g_watch_addr)) if (uint8_t *p = fast_ptr(addr)) {
      const uint64_t v = __builtin_bswap64(value); std::memcpy(p, &v, 8); return;
    }
    write64_slow(addr, value);
  }
  NWII_FORCEINLINE void write_f32(uint32_t addr, float value) {
    if (NWII_LIKELY(!g_watch_addr)) if (uint8_t *p = fast_ptr(addr)) {
      uint32_t v; std::memcpy(&v, &value, 4); v = __builtin_bswap32(v); std::memcpy(p, &v, 4); return;
    }
    write_f32_slow(addr, value);
  }
  NWII_FORCEINLINE void write_f64(uint32_t addr, double value) {
    if (NWII_LIKELY(!g_watch_addr)) if (uint8_t *p = fast_ptr(addr)) {
      uint64_t v; std::memcpy(&v, &value, 8); v = __builtin_bswap64(v); std::memcpy(p, &v, 8); return;
    }
    write_f64_slow(addr, value);
  }

  inline bool is_hw_reg(uint32_t paddr) const {
      return (paddr >= 0x0C000000 && paddr < 0x0C010000) || 
             (paddr >= 0x0D000000 && paddr < 0x0D010000);
  }

  NWII_NOINLINE uint8_t read8_slow(uint32_t addr) {
    uint32_t paddr = addr & 0x3FFFFFFF;

    if (is_hw_reg(paddr)) {
        
        uint32_t val = HW_Reg_Read32(paddr & ~3);
        int shift = 24 - ((paddr & 3) * 8);
        return (val >> shift) & 0xFF;
    }
    uint8_t *ptr = get_ptr(addr);
    return ptr ? *ptr : 0;
  }

  void custom_watch(uint32_t, uint32_t, int) {}

  NWII_NOINLINE void write8_slow(uint32_t addr, uint8_t value) {
    custom_watch(addr, value, 8);
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (g_watch_addr && (paddr & ~3u) == (g_watch_addr & 0x3FFFFFFC))
      watch_hit(addr, value, 8);

    // The whole 0x0C008xxx page mirrors the write-gather pipe (Dolphin

    
    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_Write8(value); return; } 
    if (is_hw_reg(paddr)) return; 

    uint8_t *ptr = get_ptr(addr);
    if (ptr) *ptr = value;
  }

  NWII_NOINLINE uint16_t read16_slow(uint32_t addr) {
    uint32_t paddr = addr & 0x3FFFFFFF;

    if (is_hw_reg(paddr)) return HW_Reg_Read16(paddr);
    
    uint8_t *ptr = get_ptr(addr);
    if (ptr) return (ptr[0] << 8) | ptr[1];
    return 0;
  }

  NWII_NOINLINE void write16_slow(uint32_t addr, uint16_t value) {
    custom_watch(addr, value, 16);
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (g_watch_addr && (paddr & ~3u) == (g_watch_addr & 0x3FFFFFFC))
      watch_hit(addr, value, 16);

    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_Write16(value); return; } 
    if (is_hw_reg(paddr)) { HW_Reg_Write16(paddr, value); return; }

    uint8_t *ptr = get_ptr(addr);
    if (ptr) { ptr[0] = value >> 8; ptr[1] = value & 0xFF; }
  }

  NWII_NOINLINE uint32_t read32_slow(uint32_t addr) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (paddr == 0x0000000C) return 0; 

    if (is_hw_reg(paddr)) return HW_Reg_Read32(paddr);

    uint8_t *ptr = get_ptr(addr);
    if (!ptr) return 0;
    uint32_t val = ((uint32_t)ptr[0] << 24) | ((uint32_t)ptr[1] << 16) | ((uint32_t)ptr[2] << 8) | ptr[3];

    return val;
  }

  NWII_NOINLINE void write32_slow(uint32_t addr, uint32_t value) {
    custom_watch(addr, value, 32);
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (paddr == 0x0000000C) return; 
    if (g_watch_addr && (paddr & ~3u) == (g_watch_addr & 0x3FFFFFFC))
      watch_hit(addr, value, 32);

    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_Write32(value); return; }
    if (is_hw_reg(paddr)) { HW_Reg_Write32(paddr, value); return; }

    uint8_t *ptr = get_ptr(addr);
    if (ptr) { ptr[0] = value >> 24; ptr[1] = (value >> 16) & 0xFF; ptr[2] = (value >> 8) & 0xFF; ptr[3] = value & 0xFF; }
  }

  float read_f32(uint32_t addr) { uint32_t val = read32(addr); float f; std::memcpy(&f, &val, 4); return f; }
  NWII_NOINLINE void write_f32_slow(uint32_t addr, float value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    
    uint32_t v;
    std::memcpy(&v, &value, sizeof(float));
    if (g_watch_addr && (paddr & ~3u) == (g_watch_addr & 0x3FFFFFFC))
      watch_hit(addr, v, 32);

    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_WriteF32(value); return; } 

    uint8_t *ptr = get_ptr(addr);
    if (ptr) {
      ptr[0] = v >> 24; ptr[1] = (v >> 16) & 0xFF; ptr[2] = (v >> 8) & 0xFF; ptr[3] = v & 0xFF;
    }
  }

  NWII_NOINLINE void write_f64_slow(uint32_t addr, double value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    
    uint64_t v;
    std::memcpy(&v, &value, sizeof(double));
    if (g_watch_addr && (paddr & ~7u) == (g_watch_addr & 0x3FFFFFF8))
      watch_hit(addr, v >> 32, 64);

    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_WriteF64(value); return; } 

    uint8_t *ptr = get_ptr(addr);
    if (ptr) {
      ptr[0] = v >> 56; ptr[1] = (v >> 48) & 0xFF; ptr[2] = (v >> 40) & 0xFF; ptr[3] = (v >> 32) & 0xFF;
      ptr[4] = (v >> 24) & 0xFF; ptr[5] = (v >> 16) & 0xFF; ptr[6] = (v >> 8) & 0xFF; ptr[7] = v & 0xFF;
    }
  }

  NWII_NOINLINE uint64_t read64_slow(uint32_t addr) {
    uint8_t *ptr = get_ptr(addr);
    if (!ptr) return 0;
    return ((uint64_t)ptr[0] << 56) | ((uint64_t)ptr[1] << 48) | ((uint64_t)ptr[2] << 40) | ((uint64_t)ptr[3] << 32) |
           ((uint64_t)ptr[4] << 24) | ((uint64_t)ptr[5] << 16) | ((uint64_t)ptr[6] << 8) | ptr[7];
  }
  NWII_NOINLINE void write64_slow(uint32_t addr, uint64_t value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (g_watch_addr && (paddr & ~7u) == (g_watch_addr & 0x3FFFFFF8))
      watch_hit(addr, value >> 32, 64);

    uint8_t *ptr = get_ptr(addr);
    if (!ptr) return;
    ptr[0] = value >> 56; ptr[1] = (value >> 48) & 0xFF; ptr[2] = (value >> 40) & 0xFF; ptr[3] = (value >> 32) & 0xFF;
    ptr[4] = (value >> 24) & 0xFF; ptr[5] = (value >> 16) & 0xFF; ptr[6] = (value >> 8) & 0xFF; ptr[7] = value & 0xFF;
  }
  NWII_NOINLINE double read_f64_slow(uint32_t addr) { 
      uint8_t *ptr = get_ptr(addr);
      if (ptr) {
          uint64_t val = ((uint64_t)ptr[0] << 56) | ((uint64_t)ptr[1] << 48) | ((uint64_t)ptr[2] << 40) | ((uint64_t)ptr[3] << 32) |
                         ((uint64_t)ptr[4] << 24) | ((uint64_t)ptr[5] << 16) | ((uint64_t)ptr[6] << 8) | ptr[7];
          double f; std::memcpy(&f, &val, 8); return f;
      }
      return 0.0;
  }

};

// R8 codegen: per-function view of guest memory for the generated code
// ("ctx.mmu.X(" -> "mv_.X(", one MemView per function). Same fast path and
// the same MMU slow paths as MMU's accessors, but the MEM1/MEM2 base pointers
// and the watch flag are read once per function call instead of after every
// guest store (a store through uint8_t* may alias anything, so the compiler
// reloaded mem1.data() and g_watch_addr from memory for every access). The
// vectors are sized once in MMU() and g_watch_addr is set once at startup,
// so the cached values are always the current ones.
// [r14g5] memory operand of the fastmem asm (it only tells GCC which memory the asm touches;
// the instruction itself uses [%b, %w_i, uxtw]). 0: the accessed bytes (r12fm/r13: GCC computes
// fb + ea into a register for every access, and the may_alias type clobbers everything, ctx
// included); 1: the whole 4 GiB window, may_alias; 2: the whole window with its own alias set
// (guest memory is never a CPUContext field, so GCC may keep ctx fields cached across guest
// stores; runtime code reaching guest RAM through uint8_t* still orders against it).
#ifndef NWII_FM_OPND
#define NWII_FM_OPND 1
#endif
struct FmTag { __int128 t; };
struct FmWinA { uint8_t x[0x100000000ull]; } __attribute__((may_alias));
struct FmWinT { FmTag x[0x10000000ull]; };
#if NWII_FM_OPND == 0
#define NWII_FM_INOP(MT) "m"(*(const MT *)(fb + i))
#define NWII_FM_OUTOP(MT) "=m"(*(MT *)(fb + i))
#elif NWII_FM_OPND == 1
#define NWII_FM_INOP(MT) "m"(*(const FmWinA *)fb)
#define NWII_FM_OUTOP(MT) "+m"(*(FmWinA *)fb)
#else
#define NWII_FM_INOP(MT) "m"(*(const FmWinT *)fb)
#define NWII_FM_OUTOP(MT) "+m"(*(FmWinT *)fb)
#endif
struct MemView {
  MMU &m;
  uint8_t *const b1;
  uint8_t *const b2;
  const bool watch;
  // R9 cpucode: the bound of the MEM1 test (kMem1Size - 0x10) and, for stores,
  // the same bound folded with the watch flag (0 while a NWII_WATCH watchpoint
  // is set, so every store takes the slow path as before). On AArch64 the
  // constant 0x017FFFF0 is not a compare immediate: GCC rebuilt it with
  // mov+movk before every guest access (~660 000 sites); passed through an
  // empty asm it is a value GCC keeps in a register for the whole function.
  // Each width also has its own MEM1/MEM2 load/store (no pointer phi), so the
  // base + offset folds into the load/store ([x, w, uxtw]). Same tests, same
  // fast/slow split, same order of accesses as R8. MEM2 (paddr 0x10000000 to
  // 0x13FFFFFF, i.e. paddr >> 26 == 4) is tested with encodable immediates.
  uint32_t lim1;
  uint32_t wlim1;
  NWII_FORCEINLINE explicit MemView(MMU &mm)
      : m(mm), b1(mm.mem1.data()), b2(mm.mem2.data()), watch(g_watch_addr != 0) {
    uint32_t l = MMU::kMem1Size - 0x10u;
    uint32_t w = watch ? 0u : l;
#if defined(__aarch64__) && defined(__GNUC__)
    __asm__("" : "+r"(l));
    __asm__("" : "+r"(w));
#endif
    lim1 = l;
    wlim1 = w;
  }
  NWII_FORCEINLINE uint8_t *fast_ptr(uint32_t addr) const {
    const uint32_t paddr = addr & 0x3FFFFFFF;
    if (NWII_LIKELY(paddr - 0x10u < MMU::kMem1Size - 0x10u)) return b1 + paddr;
    if (paddr - 0x10000000u < MMU::kMem2Size) return b2 + (paddr - 0x10000000u);
    return nullptr;
  }
#define NWII_MV_LOAD(T, N, SWAP, SLOW)                                               \
  const uint32_t paddr = addr & 0x3FFFFFFF;                                          \
  T v;                                                                               \
  if (NWII_LIKELY(paddr - 0x10u < lim1)) {                                           \
    std::memcpy(&v, b1 + paddr, N);                                                  \
    return SWAP(v);                                                                  \
  }                                                                                  \
  if ((paddr >> 26) == 4) { /* paddr - 0x10000000 < kMem2Size */                    \
    std::memcpy(&v, b2 + (paddr & (MMU::kMem2Size - 1)), N);                         \
    return SWAP(v);                                                                  \
  }                                                                                  \
  return m.SLOW(addr);
#define NWII_MV_STORE(T, N, SWAPPED, SLOW)                                           \
  const uint32_t paddr = addr & 0x3FFFFFFF;                                          \
  const T v = SWAPPED;                                                               \
  if (NWII_LIKELY(paddr - 0x10u < wlim1)) {                                          \
    std::memcpy(b1 + paddr, &v, N);                                                  \
    return;                                                                          \
  }                                                                                  \
  if (!watch && (paddr >> 26) == 4) {                                               \
    std::memcpy(b2 + (paddr & (MMU::kMem2Size - 1)), &v, N);                         \
    return;                                                                          \
  }                                                                                  \
  m.SLOW(addr, value);
#define NWII_MV_ID(x) (x)
  NWII_FORCEINLINE uint8_t read8_ref(uint32_t addr) { NWII_MV_LOAD(uint8_t, 1, NWII_MV_ID, read8_slow) }
  NWII_FORCEINLINE uint16_t read16_ref(uint32_t addr) { NWII_MV_LOAD(uint16_t, 2, __builtin_bswap16, read16_slow) }
  NWII_FORCEINLINE uint32_t read32_ref(uint32_t addr) { NWII_MV_LOAD(uint32_t, 4, __builtin_bswap32, read32_slow) }
  NWII_FORCEINLINE uint64_t read64_ref(uint32_t addr) { NWII_MV_LOAD(uint64_t, 8, __builtin_bswap64, read64_slow) }
  NWII_FORCEINLINE float read_f32_ref(uint32_t addr) { uint32_t val = read32_ref(addr); float f; std::memcpy(&f, &val, 4); return f; }
  NWII_FORCEINLINE double read_f64_ref(uint32_t addr) {
    const uint32_t paddr = addr & 0x3FFFFFFF;
    uint64_t v;
    double f;
    if (NWII_LIKELY(paddr - 0x10u < lim1)) {
      std::memcpy(&v, b1 + paddr, 8);
      v = __builtin_bswap64(v);
      std::memcpy(&f, &v, 8);
      return f;
    }
    if ((paddr >> 26) == 4) {
      std::memcpy(&v, b2 + (paddr & (MMU::kMem2Size - 1)), 8);
      v = __builtin_bswap64(v);
      std::memcpy(&f, &v, 8);
      return f;
    }
    return m.read_f64_slow(addr);
  }
  NWII_FORCEINLINE void write8_ref(uint32_t addr, uint8_t value) { NWII_MV_STORE(uint8_t, 1, value, write8_slow) }
  NWII_FORCEINLINE void write16_ref(uint32_t addr, uint16_t value) { NWII_MV_STORE(uint16_t, 2, __builtin_bswap16(value), write16_slow) }
  NWII_FORCEINLINE void write32_ref(uint32_t addr, uint32_t value) { NWII_MV_STORE(uint32_t, 4, __builtin_bswap32(value), write32_slow) }
  NWII_FORCEINLINE void write64_ref(uint32_t addr, uint64_t value) { NWII_MV_STORE(uint64_t, 8, __builtin_bswap64(value), write64_slow) }
  NWII_FORCEINLINE void write_f32_ref(uint32_t addr, float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    NWII_MV_STORE(uint32_t, 4, __builtin_bswap32(bits), write_f32_slow)
  }
  NWII_FORCEINLINE void write_f64_ref(uint32_t addr, double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, 8);
    NWII_MV_STORE(uint64_t, 8, __builtin_bswap64(bits), write_f64_slow)
  }
#undef NWII_MV_LOAD
#undef NWII_MV_STORE
#undef NWII_MV_ID

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
                   : [b] "r"(fb), [i] "r"(i), [id] "i"(ID), NWII_FM_INOP(MT));
#define NWII_FM_ST(ins, MT, K, VAL)                                                             \
  const uint64_t i = addr; const uint64_t sv = (VAL);                                           \
  __asm__ volatile("1: " ins "\n" NWII_FM_TAB(K)                                                \
                   : NWII_FM_OUTOP(MT)                                                      \
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
#elif NWII_FASTMEM && defined(__aarch64__)
  // Switch: same scheme, fixed form [Xbase, Wea, UXTW] (decoded by the exception entry).
  uint8_t *const fb = g_fm_base;
  struct FmB1 { uint8_t x[1]; } __attribute__((may_alias));
  struct FmB2 { uint8_t x[2]; } __attribute__((may_alias));
  struct FmB4 { uint8_t x[4]; } __attribute__((may_alias));
  struct FmB8 { uint8_t x[8]; } __attribute__((may_alias));
#define NWII_FM_TAB(K) ".pushsection nwii_fm_tab,\"a\"\n.balign 4\n.long 1b - .\n.long (%c[id] << 8) | " #K "\n.popsection\n"
#define NWII_FM_LD(ins, MT, K)                                                                  \
  uint64_t v; const uint32_t i = addr;                                                          \
  __asm__ volatile("1: " ins "\n" NWII_FM_TAB(K)                                                \
                   : [v] "=r"(v)                                                                \
                   : [b] "r"(fb), [i] "r"(i), [id] "i"(ID), NWII_FM_INOP(MT));
#define NWII_FM_ST(ins, MT, K, VAL)                                                             \
  const uint32_t i = addr; const uint64_t sv = (VAL);                                           \
  __asm__ volatile("1: " ins "\n" NWII_FM_TAB(K)                                                \
                   : NWII_FM_OUTOP(MT)                                                      \
                   : [v] "r"(sv), [b] "r"(fb), [i] "r"(i), [id] "i"(ID));
  template <uint32_t ID> NWII_FORCEINLINE uint8_t read8(uint32_t addr) {
    NWII_FM_LD("ldrb %w[v], [%[b], %w[i], uxtw]", FmB1, 1) return uint8_t(v); }
  template <uint32_t ID> NWII_FORCEINLINE uint16_t read16(uint32_t addr) {
    NWII_FM_LD("ldrh %w[v], [%[b], %w[i], uxtw]", FmB2, 2) return __builtin_bswap16(uint16_t(v)); }
  template <uint32_t ID> NWII_FORCEINLINE uint32_t read32(uint32_t addr) {
    NWII_FM_LD("ldr %w[v], [%[b], %w[i], uxtw]", FmB4, 3) return __builtin_bswap32(uint32_t(v)); }
  template <uint32_t ID> NWII_FORCEINLINE uint64_t read64(uint32_t addr) {
    NWII_FM_LD("ldr %x[v], [%[b], %w[i], uxtw]", FmB8, 4) return __builtin_bswap64(v); }
  template <uint32_t ID> NWII_FORCEINLINE float read_f32(uint32_t addr) {
    uint32_t val = read32<ID>(addr); float f; std::memcpy(&f, &val, 4); return f; }
  template <uint32_t ID> NWII_FORCEINLINE double read_f64(uint32_t addr) {
    NWII_FM_LD("ldr %x[v], [%[b], %w[i], uxtw]", FmB8, 6) v = __builtin_bswap64(v); double f; std::memcpy(&f, &v, 8); return f; }
  template <uint32_t ID> NWII_FORCEINLINE void write8(uint32_t addr, uint8_t value) {
    NWII_FM_ST("strb %w[v], [%[b], %w[i], uxtw]", FmB1, 7, value) }
  template <uint32_t ID> NWII_FORCEINLINE void write16(uint32_t addr, uint16_t value) {
    NWII_FM_ST("strh %w[v], [%[b], %w[i], uxtw]", FmB2, 8, __builtin_bswap16(value)) }
  template <uint32_t ID> NWII_FORCEINLINE void write32(uint32_t addr, uint32_t value) {
    NWII_FM_ST("str %w[v], [%[b], %w[i], uxtw]", FmB4, 9, __builtin_bswap32(value)) }
  template <uint32_t ID> NWII_FORCEINLINE void write64(uint32_t addr, uint64_t value) {
    NWII_FM_ST("str %x[v], [%[b], %w[i], uxtw]", FmB8, 10, __builtin_bswap64(value)) }
  template <uint32_t ID> NWII_FORCEINLINE void write_f32(uint32_t addr, float value) {
    uint32_t bits; std::memcpy(&bits, &value, 4);
    NWII_FM_ST("str %w[v], [%[b], %w[i], uxtw]", FmB4, 11, __builtin_bswap32(bits)) }
  template <uint32_t ID> NWII_FORCEINLINE void write_f64(uint32_t addr, double value) {
    uint64_t bits; std::memcpy(&bits, &value, 8);
    NWII_FM_ST("str %x[v], [%[b], %w[i], uxtw]", FmB8, 12, __builtin_bswap64(bits)) }
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


// Guest calls, including indirect calls and tail calls, run as native calls,
// so the host stack grows with the guest's call chain. Generated code checks
// this before a call that could recurse without bound and falls back to
// returning to run_game (which unwinds the host stack) when it is low.
//
// R9 guestloop (supersedes the r9 cpucode constinit thread_local): only the game
// CPU thread runs generated code, so its stack's red zone is two plain hidden
// globals: the check fails only for a stack pointer inside [lo, lo + span], the
// last 1 MiB of the CPU thread's stack, exactly where the r8 check failed. Any
// other thread (and every thread before cpu_thread_func sets it: 0, 0) passes,
// as with r8's thread_local limit of 0. One adrp + ldp on AArch64, no TLS.
struct NativeStackGuard { uintptr_t lo, span; };
extern NativeStackGuard g_native_stack __attribute__((visibility("hidden")));
inline bool native_stack_ok() {
  char probe;
  return (uintptr_t)&probe - g_native_stack.lo > g_native_stack.span;
}

// Called by generated code inside the SDK's "no runnable thread" spin
// (SelectThread: lwz rX,RunQueueBits(r13); cmpwi rX,0; beq): sleeps until the
// next scheduled event instead of burning the CPU, when the timebase is real.
void idle_wait(CPUContext &ctx);

// ---------------------------------------------------------------------------
// Gekko/Broadway floating point, bit-for-bit with Dolphin's interpreter
// (Interpreter_FPUtils.h, Interpreter_FloatingPoint.cpp, Interpreter_Paired.cpp,
// Common/FloatUtils.cpp). Each FPR is a pair (ps0 = fpr[], ps1 = ps1[]):
//  - single-precision scalar ops (fadds..fnmsubs, fres, frsp) and lfs* write
//    the rounded result to BOTH halves (Dolphin PairedSingle::Fill);
//  - double ops, fmr/fneg/fabs/fnabs/fsel, frsqrte, fctiw*, lfd* touch ps0 only;
//  - every single-precision multiply rounds operand C to a 25-bit mantissa
//    first (Force25Bit), and the single fused multiply-adds round only once.
// FPSCR exception bits and NaN payload/sign quirks are not modelled (no NSMBW
// code reads them; no FP instruction in the game has Rc=1).
// ---------------------------------------------------------------------------
inline uint64_t f64_bits(double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; }
inline double f64_from(uint64_t u) { double d; std::memcpy(&d, &u, 8); return d; }
inline float f32_from(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

// Dolphin Force25Bit: round frC's mantissa to 25 bits (ties away), keeping the
// double exponent; subnormals are normalised first.
inline double ppc_force25(double d) {
  uint64_t x = f64_bits(d);
  const uint64_t exp = x & 0x7FF0000000000000ULL, frac = x & 0x000FFFFFFFFFFFFFULL;
  if (__builtin_expect(exp == 0 && frac != 0, 0)) {
    int64_t keep = (int64_t)0xFFFFFFFFF8000000LL;
    uint64_t round = 0x8000000;
    const unsigned shift = (unsigned)__builtin_clzll(frac) - (63 - 52);
    keep >>= shift;
    round >>= shift;
    x = (x & (uint64_t)keep) + (x & round);
  } else {
    x = (x & 0xFFFFFFFFF8000000ULL) + (x & 0x8000000ULL);
  }
  return f64_from(x);
}

// Dolphin ForceSingle with FPSCR.NI = 0: plain IEEE round to single.
inline double ppc_single(double v) { return (double)(float)v; }

// Dolphin NI_madd_msub<sub, single=true>: a * Force25Bit(c) +/- b with a
// double FMA, then fix the direction of an exact single-precision tie so the
// later round to single happens once (as the hardware does).
inline double ppc_madd_s(double a, double c, double b_signed) {
  const double cr = ppc_force25(c);
  double r = std::fma(a, cr, b_signed);
  const uint64_t bits = f64_bits(r);
  if (__builtin_expect((bits & 0x1FFFFFFFULL) == 0x10000000ULL, 0)) {
    const double a_prime = b_signed - r;
    const double b_prime = r + a_prime;
    const double delta_a = std::fma(a, cr, a_prime);
    const double delta_b = b_signed - b_prime;
    const double error = delta_a + delta_b;
    if (error != 0.0)
      r = f64_from(((error > 0.0) == (r > 0.0)) ? bits + 1 : bits - 1);
  }
  return r;
}

// Negated single FMA result: Dolphin keeps NaNs unnegated.
inline double ppc_neg_keep_nan(double v) { return std::isnan(v) ? v : -v; }

inline double ppc_make_quiet(double v) { return f64_from(f64_bits(v) | 0x0008000000000000ULL); }

// fres / ps_res: hardware reciprocal estimate (Common::ApproximateReciprocal).
inline double ppc_fres(double val) {
  struct BaseAndDec { int32_t base, dec; };
  static constexpr BaseAndDec tbl[32] = {
      {0x7ff800, 0x3e1}, {0x783800, 0x3a7}, {0x70ea00, 0x371}, {0x6a0800, 0x340},
      {0x638800, 0x313}, {0x5d6200, 0x2ea}, {0x579000, 0x2c4}, {0x520800, 0x2a0},
      {0x4cc800, 0x27f}, {0x47ca00, 0x261}, {0x430800, 0x245}, {0x3e8000, 0x22a},
      {0x3a2c00, 0x212}, {0x360800, 0x1fb}, {0x321400, 0x1e5}, {0x2e4a00, 0x1d1},
      {0x2aa800, 0x1be}, {0x272c00, 0x1ac}, {0x23d600, 0x19b}, {0x209e00, 0x18b},
      {0x1d8800, 0x17c}, {0x1a9000, 0x16e}, {0x17ae00, 0x15b}, {0x14f800, 0x15b},
      {0x124400, 0x143}, {0x0fbe00, 0x143}, {0x0d3800, 0x12d}, {0x0ade00, 0x12d},
      {0x088400, 0x11a}, {0x065000, 0x11a}, {0x041c00, 0x108}, {0x020c00, 0x106}};
  const int64_t integral = (int64_t)f64_bits(val);
  const int64_t mantissa = integral & ((1LL << 52) - 1);
  const int64_t sign = integral & (int64_t)(1ULL << 63);
  int64_t exponent = integral & (0x7FFLL << 52);
  if (mantissa == 0 && exponent == 0)
    return std::copysign(INFINITY, val);
  if (exponent == (0x7FFLL << 52))
    return mantissa == 0 ? std::copysign(0.0, val) : ppc_make_quiet(val);
  if (exponent < (895LL << 52))
    return std::copysign((double)3.40282346638528859812e+38F, val);
  if (exponent >= (1149LL << 52))
    return std::copysign(0.0, val);
  exponent = (0x7FDLL << 52) - exponent;
  const int i = (int)(mantissa >> 37);
  const BaseAndDec &e = tbl[i / 1024];
  int64_t out = sign | exponent;
  out |= (int64_t)(e.base - (e.dec * (i % 1024) + 1) / 2) << 29;
  return f64_from((uint64_t)out);
}

// frsqrte / ps_rsqrte estimate (Common::ApproximateReciprocalSquareRoot).
inline double ppc_frsqrte(double val) {
  struct BaseAndDec { int32_t base, dec; };
  static constexpr BaseAndDec tbl[32] = {
      {0x1a7e800, -0x568}, {0x17cb800, -0x4f3}, {0x1552800, -0x48d}, {0x130c000, -0x435},
      {0x10f2000, -0x3e7}, {0x0eff000, -0x3a2}, {0x0d2e000, -0x365}, {0x0b7c000, -0x32e},
      {0x09e5000, -0x2fc}, {0x0867000, -0x2d0}, {0x06ff000, -0x2a8}, {0x05ab800, -0x283},
      {0x046a000, -0x261}, {0x0339800, -0x243}, {0x0218800, -0x226}, {0x0105800, -0x20b},
      {0x3ffa000, -0x7a4}, {0x3c29000, -0x700}, {0x38aa000, -0x670}, {0x3572000, -0x5f2},
      {0x3279000, -0x584}, {0x2fb7000, -0x524}, {0x2d26000, -0x4cc}, {0x2ac0000, -0x47e},
      {0x2881000, -0x43a}, {0x2665000, -0x3fa}, {0x2468000, -0x3c2}, {0x2287000, -0x38e},
      {0x20c1000, -0x35e}, {0x1f12000, -0x332}, {0x1d79000, -0x30a}, {0x1bf4000, -0x2e6}};
  int64_t integral = (int64_t)f64_bits(val);
  int64_t mantissa = integral & ((1LL << 52) - 1);
  const int64_t sign = integral & (int64_t)(1ULL << 63);
  int64_t exponent = integral & (0x7FFLL << 52);
  if (mantissa == 0 && exponent == 0)
    return sign ? -INFINITY : INFINITY;
  if (exponent == (0x7FFLL << 52)) {
    if (mantissa == 0)
      return sign ? std::numeric_limits<double>::quiet_NaN() : 0.0;
    return ppc_make_quiet(val);
  }
  if (sign)
    return std::numeric_limits<double>::quiet_NaN();
  if (!exponent) {
    do {
      exponent -= 1LL << 52;
      mantissa <<= 1;
    } while (!(mantissa & (1LL << 52)));
    mantissa &= (1LL << 52) - 1;
    exponent += 1LL << 52;
  }
  const int64_t exponent_lsb = exponent & (1LL << 52);
  exponent = ((0x3FFLL << 52) - ((exponent - (0x3FELL << 52)) / 2)) & (0x7FFLL << 52);
  integral = sign | exponent;
  const int i = (int)((exponent_lsb | mantissa) >> 37);
  const BaseAndDec &e = tbl[i / 2048];
  integral |= (int64_t)(e.base + e.dec * (i % 2048)) << 26;
  return f64_from((uint64_t)integral);
}

// stfs/stfsx/stfsu/stfsux: the hardware does not round, it selects bits
// (Dolphin ConvertToSingle), and denormalises tiny values.
inline uint32_t ppc_single_bits(double d) {
  const uint64_t x = f64_bits(d);
  const uint32_t exp = (uint32_t)((x >> 52) & 0x7ff);
  if (exp > 896 || (x & ~(1ULL << 63)) == 0)
    return (uint32_t)(((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff));
  if (exp >= 874) {
    uint32_t t = (uint32_t)(0x80000000 | ((x & 0x000FFFFFFFFFFFFFULL) >> 21));
    t = t >> (905 - exp);
    t |= (uint32_t)((x >> 32) & 0x80000000);
    return t;
  }
  return (uint32_t)(((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff));
}
inline float ppc_to_single(double d) { return f32_from(ppc_single_bits(d)); }

// psq_st with a float GQR type (Dolphin ConvertToSingleFTZ).
inline uint32_t ppc_single_bits_ftz(double d) {
  const uint64_t x = f64_bits(d);
  const uint32_t exp = (uint32_t)((x >> 52) & 0x7ff);
  if (exp > 896 || (x & ~(1ULL << 63)) == 0)
    return (uint32_t)(((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff));
  return (uint32_t)((x >> 32) & 0x80000000);
}

// fctiw/fctiwz: Gekko saturates out-of-range results to 0x7FFFFFFF /
// 0x80000000 and turns NaN into 0x80000000. A plain C++ cast is undefined
// there, and on x86 large positives come out as 0x80000000 (seen as a
// 2^31 float feeding NSMBW's collision and angle code, which then spins).
inline uint32_t ppc_to_s32_sat(double v) {
  if (std::isnan(v)) return 0x80000000u;
  if (v >= 2147483647.0) return 0x7FFFFFFFu;
  if (v <= -2147483648.0) return 0x80000000u;
  return (uint32_t)(int32_t)v;
}
inline uint32_t ppc_fctiwz(double v) { return ppc_to_s32_sat(std::trunc(v)); }
inline uint32_t ppc_fctiw(double v) { return ppc_to_s32_sat(std::nearbyint(v)); }
// fctiw with FPSCR[RN] (0 nearest-even, 1 toward 0, 2 +inf, 3 -inf).
inline uint32_t ppc_fctiw_rn(double v, uint32_t rn) {
  switch (rn & 3) {
  case 1: return ppc_to_s32_sat(std::trunc(v));
  case 2: return ppc_to_s32_sat(std::ceil(v));
  case 3: return ppc_to_s32_sat(std::floor(v));
  default: return ppc_to_s32_sat(std::nearbyint(v));
  }
}
// The FPR image of an fctiw* result (Dolphin, from hardware tests): the
// integer in the low word, 0xFFF80000 above it, plus bit 32 for a -0 input.
inline uint64_t ppc_fctiw_image(uint32_t value, double src) {
  uint64_t r = 0xFFF8000000000000ULL | value;
  if (value == 0 && std::signbit(src)) r |= 0x100000000ULL;
  return r;
}

// divw: Dolphin's results for the undefined cases (x / 0 and INT_MIN / -1,
// the latter traps in an x86 idiv): -1 for a negative dividend, else 0.
inline uint32_t ppc_divw(uint32_t a, uint32_t b) {
  if (b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu))
    return ((int32_t)a < 0) ? 0xFFFFFFFFu : 0u;
  return (uint32_t)((int32_t)a / (int32_t)b);
}

// GQR scale fields are 6-bit two's complement (-32..31).
inline int gqr_scale(uint32_t field) {
  int s = (int)(field & 0x3F);
  return (s & 0x20) ? s - 64 : s;
}
// 2^n for the dequantization scale (|n| <= 32): the double's bit pattern
// directly, identical to std::ldexp(1.0, n) (exact power of two) without the
// libm call (scalbn was 1.3% of the CPU thread in the R8 profile).
inline double gqr_pow2(int n) {
  const uint64_t bits = (uint64_t)(1023 + n) << 52;
  double d;
  std::memcpy(&d, &bits, 8);
  return d;
}

// Quantized stores saturate to the destination type, like the hardware
// (Dolphin ScaleAndClamp: the value is rounded to single, scaled in single
// precision, clamped, then truncated toward zero).
inline int32_t ppc_quantize(double v, double scale, double lo, double hi) {
  float f = (float)v * (float)scale;
  if (std::isnan(f)) return 0;
  if (f < (float)lo) f = (float)lo;
  if (f > (float)hi) f = (float)hi;
  return (int32_t)f;
}

struct CPUContext {
  
  std::array<uint32_t, 32> gpr;

  std::array<double, 32> fpr;

  std::array<double, 32> ps1;

  std::array<ConditionField, 8> cr;

  uint32_t pc;    
  uint32_t lr;    
  uint32_t ctr;   
  uint32_t xer;   
  uint32_t msr;   
  uint32_t fpscr; 
  uint32_t srr0;  
  uint32_t srr1;  

  std::array<uint32_t, 8> gqr;
  std::array<uint32_t, 4> sprg;

  // HID2 (SPR 920): LSQE|WPE|PSE after boot (Dolphin SetupHID); LCEnable sets
  // LCE. DMAQL (bits 4-7) stays 0 because LC DMAs complete synchronously.
  uint32_t hid2 = 0xE0000000u;
  // Locked-cache DMA registers (SPR 922 DMA_U, 923 DMA_L).
  uint32_t dma_u = 0, dma_l = 0;

  struct BackupState {
    std::array<uint32_t, 32> gpr;
    std::array<double, 32> fpr;
    std::array<double, 32> ps1;
    std::array<ConditionField, 8> cr;
    uint32_t lr;
    uint32_t ctr;
    uint32_t xer;
    uint32_t pc;
    uint32_t srr0;
    uint32_t srr1;
    uint32_t msr;
    uint32_t fpscr;
    std::array<uint32_t, 8> gqr;
    std::array<uint32_t, 4> sprg;
  };
  std::stack<BackupState> backup_stack;
  bool in_callback = false;
  int callback_depth = 0;

  MMU mmu;

  uint32_t exception_pc;
  uint64_t inst_count = 0;
  
  std::atomic<bool> is_running;
  std::atomic<bool> vblank_pending;
  std::mutex cb_mutex;
  std::queue<CallbackInfo> pending_callbacks;
  
  std::atomic<int> pending_cb_count{0};
  jmp_buf exception_jmp_buf;

  uint32_t reservation_addr = 0xFFFFFFFF;

  

  uint32_t dispatch_saved_ctx = 0;

  
  bool ext_resched_pending = false;

  



  

  
  uint64_t tb_freq = 40500000;
  std::chrono::steady_clock::time_point tb_start =
      std::chrono::steady_clock::now();

  

  
  // Instruction count is a virtual clock: the guest gets a fixed number of
  // instructions per emulated frame and never falls behind. Real elapsed time
  // (NWII_WALLTB=1) is the hardware-accurate source, but while we run slower
  // than real time it starves the game — measured on MP7: same input script,
  // 122 DVD reads with the virtual clock vs 52 with the wall clock. Switch the
  // default once the drain is fast enough to keep up.
  static bool wall_clock_tb() {
    // NWII_REALTIME=1 also selects the wall clock (and, in main.cpp, drops
    // the host-loop VI so retraces come only from this timebase at 60 Hz).
    static const bool wall = NWII_ENV("NWII_WALLTB") || NWII_ENV("NWII_REALTIME");
    return wall;
  }

  // R9 guestloop: out of line (os_hle_stubs.cpp), so the host clock source
  // can change without recompiling the generated code. PC: steady_clock in
  // microseconds, as before. Switch: the system tick counter (NWII_TB_SRC).
  NWII_NOINLINE uint64_t wall_timebase_now() const;

  // A clock read costs ~25 ns and the interrupt poll on every guest back-edge
  // reads the timebase up to three times (31% of the CPU thread in a profile).
  // It is refreshed every 32 reads; the guest only sees time advance in
  // slightly coarser steps. tb_invalidate() forces a fresh read (after a sleep).
  mutable uint64_t tb_cached = 0;
  mutable uint32_t tb_reads = 0;
  void tb_invalidate() { tb_reads = 0; poll_left.store(1, std::memory_order_relaxed); }

  // ---- R9 guestloop: interrupt poll countdown ---------------------------
  // process_pending_callbacks() runs at every guest back-edge (~3 M/s). Its
  // inline part only decrements poll_left and, with MSR[EE] set, looks for
  // a pending external interrupt, a queued callback or an expired
  // decrementer (poll_dec_due); everything else (host clock refresh,
  // scheduler events, the rare cases) waits until poll_left runs out, every
  // kPollTbEvery polls: as often as r8 refreshed the clock (every 32 timebase
  // reads, two per poll), and the guest time only changes at a refresh.
  // Whatever can change the outcome before that kicks the countdown
  // (poll_kick: decrementer writes, a timebase refresh elsewhere, an event
  // scheduled for now, IPC/DI interrupt delays, the legacy callback path),
  // so the next poll takes the full path, as r8 would have. Written by other
  // threads only to kick it (main.cpp, virtual-clock VI), hence atomic.
  static constexpr int32_t kPollTbEvery = 16;
  static constexpr int32_t kPollKick = -(1 << 30);  // < 0: next poll is slow, no clock refresh
  std::atomic<int32_t> poll_left{0};
  bool poll_dec_due = false;  // decrementer expired at the last full poll
  void poll_kick() { poll_left.store(kPollKick, std::memory_order_relaxed); }

  // ---- Overload governor (real-time clock only) ---------------------------
  // The wall clock drives the AX audio DMA and DSP mails (~1000 interrupts/s),
  // VI and the OS alarms. That work costs the same every real second however
  // slow the host is. On the Switch the AX interrupts alone grew to the whole
  // CPU thread: every context restore went straight into the next AI/DSP
  // interrupt, the interrupted thread (nw4r snd task thread, prio 3) never got
  // past its back-edge and the game threads starved (0 fps for good).
  // The governor lets the guest clock fall behind the wall clock ("slip") while
  // the guest has not idled for a long time, so the game runs slower instead of
  // starving, and brings it back to real time once the guest idles again. A
  // host that keeps up idles every frame: the rate stays 1 and the guest clock
  // is exactly the wall clock (no slip). See governor_step (os_hle_stubs.cpp).
  static constexpr uint32_t kGovOne = 1u << 16;
  mutable uint64_t gov_slip = 0;       // ticks the guest clock is behind the wall
  mutable uint64_t gov_wall = 0;       // wall timebase at the last refresh
  mutable uint64_t gov_step_wall = 0;  // wall timebase of the last governor step
  mutable uint64_t gov_idle_wall = 0;  // wall timebase when the guest last idled
  mutable uint32_t gov_rate = kGovOne; // guest clock speed, Q16 (kGovOne = real time)
  NWII_NOINLINE void governor_step(uint64_t wall) const;
  // The OS idle loop / a guest polling loop: nothing to run until an event.
  void note_idle() const { gov_idle_wall = gov_wall; }

  // Wall timebase -> guest timebase (applies and advances the slip).
  uint64_t guest_timebase_from_wall(uint64_t wall) const {
    if (wall > gov_wall) {
      if (gov_rate < kGovOne) {
        const uint64_t d = wall - gov_wall;
        gov_slip += d - ((d * gov_rate) >> 16);
      }
      gov_wall = wall;
      if (wall - gov_step_wall >= tb_freq / 100)  // every 10 ms
        governor_step(wall);
    }
    return wall - gov_slip;
  }

  uint64_t read_timebase() const {
    if (wall_clock_tb()) {
      static const uint32_t refresh_mask = NWII_ENV("NWII_TB_NOCACHE") ? 0 : 31;
      if ((tb_reads++ & refresh_mask) == 0) {
        uint64_t tb = guest_timebase_from_wall(wall_timebase_now());
        if (tb > tb_cached) {
          tb_cached = tb;
          const_cast<CPUContext *>(this)->poll_kick();  // R9: the poll sees the new time
        }
      }
      return tb_cached;
    }
    return inst_count;
  }

  
  uint64_t dec_value = 0xFFFFFFFF;
  uint64_t dec_written_tb = 0;
  bool dec_irq_pending = false;

  void write_dec(uint32_t v) {
    if (NWII_ENV("NWII_SAMPLE")) {
      static uint64_t n = 0;
      ++n;
      if (n <= 10 || (n % 500) == 0)
        std::cout << "[DEC] mtdec #" << n << " val=" << v << " pc=0x"
                  << std::hex << pc << " lr=0x" << lr << std::dec
                  << " inst=" << inst_count << "\n";
    }
    dec_value = v;
    dec_written_tb = read_timebase();
    dec_irq_pending = (v != 0xFFFFFFFF);
    poll_kick();  // R9 guestloop: recompute poll_dec_due at the next poll
  }

  uint32_t read_dec() {
    uint64_t elapsed = read_timebase() - dec_written_tb;
    if (elapsed >= dec_value)
      return 0;
    return (uint32_t)(dec_value - elapsed);
  }

  bool dec_expired() {
    return dec_irq_pending && (read_timebase() - dec_written_tb) >= dec_value;
  }

  CPUContext() : gpr{0}, fpr{0.0}, ps1{0.0}, cr{}, pc(0), lr(0), ctr(0), xer(0), 
                 msr(0), fpscr(0), srr0(0), srr1(0), gqr{0}, exception_pc(0), is_running(true), vblank_pending(false) {}

  void queue_callback(uint32_t cb, uint32_t arg1, uint32_t arg2, bool is_irq = false) {
    std::lock_guard<std::mutex> lock(cb_mutex);
    pending_callbacks.push({cb, arg1, arg2, is_irq});
    pending_cb_count.fetch_add(1, std::memory_order_relaxed);
    poll_kick();  // [r33-g7] sondeo compacto (g7::poll_due no mira pending_cb_count): fuerza el sondeo lento
  }

  // ---- Locked cache (LC) -------------------------------------------------
  // mtspr HID2 (Dolphin: only 0xF0FF0000 is writable and DMAQL is kept).
  void write_hid2(uint32_t v) { hid2 = (v & 0xF0FF0000u) | (hid2 & 0x0F000000u); }

  // mtspr DMA_L (Dolphin Interpreter_SystemRegisters.cpp SPR_DMAL, MMU.cpp
  // DMA_LCToMemory/DMA_MemoryToLC):
  //   DMA_U = MEM_ADDR[0:26] | LEN_U[27:31]
  //   DMA_L = LC_ADDR[0:26]  | LD[27] | LEN_L[28:29] | T[30] | F[31]
  // With T set, move ((LEN_U << 2) | LEN_L) 32-byte lines (0 = 128) between
  // memory (physical, the SDK masks it with 0x1FFFFFFF) and the locked cache;
  // LD = 1 is memory -> cache. The copy is instantaneous, T is cleared and the
  // HID2 DMA queue (LCQueueLength) reads back empty.
  void write_dmal(uint32_t v) {
    dma_l = v;
    if (v & 0x2) {
      const uint32_t mem_addr = dma_u & ~0x1Fu;
      const uint32_t lc_addr = v & ~0x1Fu;
      uint32_t lines = ((dma_u & 0x1F) << 2) | ((v >> 2) & 3);
      if (lines == 0)
        lines = 128;
      const bool to_lc = (v & 0x10) != 0;
      for (uint32_t i = 0; i < lines * 32; i += 32) {
        uint8_t *lc = &mmu.lc[(lc_addr + i) & (MMU::kLCSize - 1)];
        uint8_t *mem = mmu.get_ptr((mem_addr + i) & 0x3FFFFFFF);
        if (!mem)
          continue;  // EFB / MMIO targets: unused by the SDK LC helpers
        if (to_lc)
          std::memcpy(lc, mem, 32);
        else
          std::memcpy(mem, lc, 32);
      }
    }
    dma_l &= ~0x2u;
  }

  // dcbz / dcbz_l: zero the 32-byte block holding ea (Dolphin
  // MMU::ClearDCacheLine). dcbz_l is how LCEnable clears the locked cache.
  void dcbz(uint32_t ea) {
    ea &= ~31u;
    uint8_t *p = mmu.get_ptr(ea);
    if (p && mmu.get_ptr(ea + 31) == p + 31) {
      std::memset(p, 0, 32);
      return;
    }
    for (uint32_t i = 0; i < 32; i += 4)
      mmu.write32(ea + i, 0);
  }

  // ---- FPSCR ---------------------------------------------------------------
  // Stored and read back (mffs/mtfsf/mtfsfi/mtfsb0/mtfsb1); RN drives fctiw.
  // Host arithmetic stays round-to-nearest, without NI flush-to-zero.
  void set_fpscr(uint32_t v) {
    if ((v & 0x7) != 0 && (fpscr & 0x7) != (v & 0x7) && NWII_ENV("NWII_FPSCR_LOG"))
      std::cerr << "[FPSCR] RN/NI set: 0x" << std::hex << v << std::dec << "\n";
    fpscr = v & ~0x40000000u;  // FEX is a summary bit, never set directly
  }
  void mffs(uint32_t D) {  // Dolphin: 0xFFF80000 in the high word
    uint64_t v = 0xFFF8000000000000ULL | fpscr;
    std::memcpy(&fpr[D], &v, 8);
  }
  void mtfsf(uint32_t fm, uint32_t B) {
    uint64_t bits;
    std::memcpy(&bits, &fpr[B], 8);
    uint32_t m = 0;
    for (int i = 0; i < 8; i++)
      if (fm & (1u << i)) m |= 0xFu << (4 * i);
    set_fpscr((fpscr & ~m) | ((uint32_t)bits & m));
  }
  void mtfsfi(uint32_t crfD, uint32_t imm) {
    const uint32_t shift = 4 * (7 - crfD);
    set_fpscr((fpscr & ~(0xFu << shift)) | ((imm & 0xF) << shift));
  }
  void mtfsb(uint32_t bit, bool one) {
    const uint32_t m = 0x80000000u >> bit;
    set_fpscr(one ? (fpscr | m) : (fpscr & ~m));
  }

  // ---- Scalar floating point (Dolphin Interpreter_FloatingPoint.cpp) -------
  inline void fill(uint32_t D, double v) { fpr[D] = v; ps1[D] = v; }
  inline void set_bits(uint32_t D, uint64_t v) { std::memcpy(&fpr[D], &v, 8); }
  // Single precision: both halves get the rounded result.
  inline void fadds(uint32_t D, uint32_t A, uint32_t B) { fill(D, ppc_single(fpr[A] + fpr[B])); }
  inline void fsubs(uint32_t D, uint32_t A, uint32_t B) { fill(D, ppc_single(fpr[A] - fpr[B])); }
  inline void fmuls(uint32_t D, uint32_t A, uint32_t C) { fill(D, ppc_single(fpr[A] * ppc_force25(fpr[C]))); }
  inline void fdivs(uint32_t D, uint32_t A, uint32_t B) { fill(D, ppc_single(fpr[A] / fpr[B])); }
  inline void fres(uint32_t D, uint32_t B) { fill(D, ppc_fres(fpr[B])); }
  inline void fmadds(uint32_t D, uint32_t A, uint32_t C, uint32_t B) { fill(D, ppc_single(ppc_madd_s(fpr[A], fpr[C], fpr[B]))); }
  inline void fmsubs(uint32_t D, uint32_t A, uint32_t C, uint32_t B) { fill(D, ppc_single(ppc_madd_s(fpr[A], fpr[C], -fpr[B]))); }
  inline void fnmadds(uint32_t D, uint32_t A, uint32_t C, uint32_t B) { fill(D, ppc_neg_keep_nan(ppc_single(ppc_madd_s(fpr[A], fpr[C], fpr[B])))); }
  inline void fnmsubs(uint32_t D, uint32_t A, uint32_t C, uint32_t B) { fill(D, ppc_neg_keep_nan(ppc_single(ppc_madd_s(fpr[A], fpr[C], -fpr[B])))); }
  inline void frsp(uint32_t D, uint32_t B) { fill(D, ppc_single(fpr[B])); }
  // Double precision and moves: ps0 only.
  inline void fmadd(uint32_t D, uint32_t A, uint32_t C, uint32_t B) { fpr[D] = std::fma(fpr[A], fpr[C], fpr[B]); }
  inline void fmsub(uint32_t D, uint32_t A, uint32_t C, uint32_t B) { fpr[D] = std::fma(fpr[A], fpr[C], -fpr[B]); }
  inline void fnmadd(uint32_t D, uint32_t A, uint32_t C, uint32_t B) { fpr[D] = ppc_neg_keep_nan(std::fma(fpr[A], fpr[C], fpr[B])); }
  inline void fnmsub(uint32_t D, uint32_t A, uint32_t C, uint32_t B) { fpr[D] = ppc_neg_keep_nan(std::fma(fpr[A], fpr[C], -fpr[B])); }
  inline void frsqrte(uint32_t D, uint32_t B) { fpr[D] = ppc_frsqrte(fpr[B]); }
  inline void fctiw(uint32_t D, uint32_t B) { set_bits(D, ppc_fctiw_image(ppc_fctiw_rn(fpr[B], fpscr), fpr[B])); }
  inline void fctiwz(uint32_t D, uint32_t B) { set_bits(D, ppc_fctiw_image(ppc_fctiwz(fpr[B]), fpr[B])); }
  inline void fcmp(uint32_t crD, double a, double b) {
    const bool un = std::isnan(a) || std::isnan(b);
    cr[crD].lt = !un && a < b;
    cr[crD].gt = !un && a > b;
    cr[crD].eq = !un && a == b;
    cr[crD].so = un;
  }
  // Loads/stores of singles. lfs fills both halves; stfs selects bits.
  inline void lfs(uint32_t D, uint32_t ea) { fill(D, (double)mmu.read_f32(ea)); }
  inline void stfs(uint32_t S, uint32_t ea) { mmu.write_f32(ea, ppc_to_single(fpr[S])); }
  inline void stfiwx(uint32_t S, uint32_t ea) {
    uint64_t bits;
    std::memcpy(&bits, &fpr[S], 8);
    mmu.write32(ea, (uint32_t)bits);
  }

  // ---- Quantized load/store (Dolphin Interpreter_LoadStorePaired.cpp) ------
  void psq_load(uint32_t frD, uint32_t addr, uint32_t W, uint32_t I) {
    uint32_t gqr_val = gqr[I];
    uint32_t ld_type = (gqr_val >> 16) & 0x7;
    const float scale = (float)gqr_pow2(-gqr_scale(gqr_val >> 24));

    auto load_element = [&](uint32_t a) -> double {
      switch (ld_type) {
      case 0:
        return mmu.read_f32(a);
      case 4:
        return (float)mmu.read8(a) * scale;
      case 5:
        return (float)mmu.read16(a) * scale;
      case 6:
        return (float)(int8_t)mmu.read8(a) * scale;
      case 7:
        return (float)(int16_t)mmu.read16(a) * scale;
      default:
        return 0.0;
      }
    };

    uint32_t elem_size =
        (ld_type == 0) ? 4 : ((ld_type == 4 || ld_type == 6) ? 1 : 2);

    const double p0 = load_element(addr);
    const double p1 = (W == 0) ? load_element(addr + elem_size) : 1.0;
    fpr[frD] = p0;
    ps1[frD] = p1;
  }

  void psq_store(uint32_t frS, uint32_t addr, uint32_t W, uint32_t I) {
    uint32_t gqr_val = gqr[I];
    uint32_t st_type = gqr_val & 0x7;
    const double scale = gqr_pow2(gqr_scale(gqr_val >> 8));

    auto store_element = [&](uint32_t a, double val) {
      switch (st_type) {
      case 0:
        mmu.write_f32(a, f32_from(ppc_single_bits_ftz(val)));
        break;
      case 4:
        mmu.write8(a, (uint8_t)ppc_quantize(val, scale, 0, 255));
        break;
      case 5:
        mmu.write16(a, (uint16_t)ppc_quantize(val, scale, 0, 65535));
        break;
      case 6:
        mmu.write8(a, (uint8_t)(int8_t)ppc_quantize(val, scale, -128, 127));
        break;
      case 7:
        mmu.write16(a, (uint16_t)(int16_t)ppc_quantize(val, scale, -32768, 32767));
        break;
      }
    };

    uint32_t elem_size =
        (st_type == 0) ? 4 : ((st_type == 4 || st_type == 6) ? 1 : 2);

    const double p0 = fpr[frS], p1 = ps1[frS];
    store_element(addr, p0);
    if (W == 0) {
      store_element(addr + elem_size, p1);
    }
  }

  // ---- Paired singles (Dolphin Interpreter_Paired.cpp) ----------------------
  // Results are rounded to single; products round operand C to 25 bits.
  // Inputs are read before the destination is written (D may alias A/B/C).
  inline void set_ps(uint32_t D, double p0, double p1) { fpr[D] = p0; ps1[D] = p1; }
  inline void ps_add(uint32_t D, uint32_t A, uint32_t B) {
    set_ps(D, ppc_single(fpr[A] + fpr[B]), ppc_single(ps1[A] + ps1[B]));
  }
  inline void ps_sub(uint32_t D, uint32_t A, uint32_t B) {
    set_ps(D, ppc_single(fpr[A] - fpr[B]), ppc_single(ps1[A] - ps1[B]));
  }
  inline void ps_mul(uint32_t D, uint32_t A, uint32_t C) {
    set_ps(D, ppc_single(fpr[A] * ppc_force25(fpr[C])), ppc_single(ps1[A] * ppc_force25(ps1[C])));
  }
  inline void ps_div(uint32_t D, uint32_t A, uint32_t B) {
    set_ps(D, ppc_single(fpr[A] / fpr[B]), ppc_single(ps1[A] / ps1[B]));
  }
  inline void ps_madd(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    set_ps(D, ppc_single(ppc_madd_s(fpr[A], fpr[C], fpr[B])),
           ppc_single(ppc_madd_s(ps1[A], ps1[C], ps1[B])));
  }
  inline void ps_msub(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    set_ps(D, ppc_single(ppc_madd_s(fpr[A], fpr[C], -fpr[B])),
           ppc_single(ppc_madd_s(ps1[A], ps1[C], -ps1[B])));
  }
  inline void ps_nmadd(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    set_ps(D, ppc_neg_keep_nan(ppc_single(ppc_madd_s(fpr[A], fpr[C], fpr[B]))),
           ppc_neg_keep_nan(ppc_single(ppc_madd_s(ps1[A], ps1[C], ps1[B]))));
  }
  inline void ps_nmsub(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    set_ps(D, ppc_neg_keep_nan(ppc_single(ppc_madd_s(fpr[A], fpr[C], -fpr[B]))),
           ppc_neg_keep_nan(ppc_single(ppc_madd_s(ps1[A], ps1[C], -ps1[B]))));
  }
  inline void ps_sum0(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    set_ps(D, ppc_single(fpr[A] + ps1[B]), ppc_single(ps1[C]));
  }
  inline void ps_sum1(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    set_ps(D, ppc_single(fpr[C]), ppc_single(fpr[A] + ps1[B]));
  }
  inline void ps_muls0(uint32_t D, uint32_t A, uint32_t C) {
    const double c0 = ppc_force25(fpr[C]);
    set_ps(D, ppc_single(fpr[A] * c0), ppc_single(ps1[A] * c0));
  }
  inline void ps_muls1(uint32_t D, uint32_t A, uint32_t C) {
    const double c1 = ppc_force25(ps1[C]);
    set_ps(D, ppc_single(fpr[A] * c1), ppc_single(ps1[A] * c1));
  }
  inline void ps_madds0(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    const double c0 = fpr[C];
    set_ps(D, ppc_single(ppc_madd_s(fpr[A], c0, fpr[B])),
           ppc_single(ppc_madd_s(ps1[A], c0, ps1[B])));
  }
  inline void ps_madds1(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    const double c1 = ps1[C];
    set_ps(D, ppc_single(ppc_madd_s(fpr[A], c1, fpr[B])),
           ppc_single(ppc_madd_s(ps1[A], c1, ps1[B])));
  }
  inline void ps_res(uint32_t D, uint32_t B) {
    set_ps(D, ppc_fres(fpr[B]), ppc_fres(ps1[B]));
  }
  inline void ps_rsqrte(uint32_t D, uint32_t B) {
    set_ps(D, ppc_single(ppc_frsqrte(fpr[B])), ppc_single(ppc_frsqrte(ps1[B])));
  }
  inline void ps_sel(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    set_ps(D, (fpr[A] >= -0.0) ? fpr[C] : fpr[B], (ps1[A] >= -0.0) ? ps1[C] : ps1[B]);
  }
  inline void ps_mr(uint32_t D, uint32_t B) {
    fpr[D] = fpr[B];
    ps1[D] = ps1[B];
  }
  inline void ps_neg(uint32_t D, uint32_t B) {
    fpr[D] = -fpr[B];
    ps1[D] = -ps1[B];
  }
  inline void ps_abs(uint32_t D, uint32_t B) {
    fpr[D] = std::abs(fpr[B]);
    ps1[D] = std::abs(ps1[B]);
  }
  inline void ps_nabs(uint32_t D, uint32_t B) {
    fpr[D] = -std::abs(fpr[B]);
    ps1[D] = -std::abs(ps1[B]);
  }

  
  inline void ps_merge00(uint32_t D, uint32_t A, uint32_t B) {
    double a = fpr[A], b = fpr[B];
    fpr[D] = a; ps1[D] = b;
  }
  inline void ps_merge01(uint32_t D, uint32_t A, uint32_t B) {
    double a = fpr[A], b = ps1[B];
    fpr[D] = a; ps1[D] = b;
  }
  inline void ps_merge10(uint32_t D, uint32_t A, uint32_t B) {
    double a = ps1[A], b = fpr[B];
    fpr[D] = a; ps1[D] = b;
  }
  inline void ps_merge11(uint32_t D, uint32_t A, uint32_t B) {
    double a = ps1[A], b = ps1[B];
    fpr[D] = a; ps1[D] = b;
  }
  inline void ps_cmpu0(uint32_t crD, uint32_t A, uint32_t B) { fcmp(crD, fpr[A], fpr[B]); }
  inline void ps_cmpo0(uint32_t crD, uint32_t A, uint32_t B) { fcmp(crD, fpr[A], fpr[B]); }
  inline void ps_cmpu1(uint32_t crD, uint32_t A, uint32_t B) { fcmp(crD, ps1[A], ps1[B]); }
  inline void ps_cmpo1(uint32_t crD, uint32_t A, uint32_t B) { fcmp(crD, ps1[A], ps1[B]); }
};

// R9 guestloop: the per-back-edge poll (see poll_left above). In generated
// code ctx.pc was just set to a constant, so the first test folds away.
// [r14-g5] split in two: poll_due() is the inline countdown/check (reads pc, msr and the
// pending flags, none of the guest registers the G5 pass keeps in locals) and says whether the
// slow path must run; the generated code writes its dirty registers back only then.
NWII_FORCEINLINE bool poll_due(CPUContext &ctx) {
  const uint32_t pc = ctx.pc;
  const int32_t left = ctx.poll_left.load(std::memory_order_relaxed) - 1;
  ctx.poll_left.store(left, std::memory_order_relaxed);
  if (NWII_UNLIKELY((pc + 4u <= 4u) | (left <= 0)))  // 0xFFFFFFFC / 0: dispatcher cases
    return true;
  if (!(ctx.msr & 0x8000)) return false;
  if (NWII_UNLIKELY(ctx.poll_dec_due |
                    ((hw::pi_intsr.load(std::memory_order_relaxed) &
                      hw::pi_intmr.load(std::memory_order_relaxed)) != 0) |
                    (ctx.pending_cb_count.load(std::memory_order_relaxed) != 0)))
    return true;
  return false;
}
inline bool process_pending_callbacks(CPUContext &ctx) {
  return poll_due(ctx) && process_pending_callbacks_slow(ctx);
}

void hle_set_ipc_arm_msg(uint32_t req_addr);
void micro_interpret(CPUContext& ctx, uint32_t opcode, uint32_t pc);

bool interpret_one(CPUContext& ctx);
void interpret_step(CPUContext& ctx);
// R8 codegen: entry at a guest address whose switch case the R8 pass pruned
// (nothing resumes there). Same as interpret_step (what the base did for any
// address without a case), plus a counter/log for the smoke tests.
void resume_pruned(CPUContext& ctx);
void add_recompiled_range(uint32_t start, uint32_t end);
bool in_recompiled_code(uint32_t pc);

extern bool g_trace_calls NWII_HIDDEN;
void trace_call(uint32_t func_addr, CPUContext& ctx);




void note_null_call(uint32_t site, uint32_t lr);

// ---- Per-thread FP register file ------------------------------------------
// On Broadway every OS thread owns its FPRs/PS1/FPSCR through the SDK's lazy
// FPU switch: OSSetCurrentContext clears MSR[FP] for any context that is not
// __OSFPUContext (0x800000D8), and the FP-unavailable exception
// (OSSwitchFPUContext) saves the owner's registers into its OSContext and
// reloads the new thread's (Dolphin emulates this exactly). Generated code
// never checks MSR[FP], so that trap never fires and the FP state of one
// thread leaked into the next one to run: the main thread, preempted by the
// sound/streaming threads, resumed with their f0-f31 in the middle of a
// function (nw4r::math angle reduction fed inf -> endless loop at
// 0x80238308; a player's mPos.y of 1.6e8 -> endless BG scan at 0x80075C18).
// The switch is done eagerly at the two points every thread switch passes:
//   OSSaveContext(c)  the thread leaves: save its FP state into c and mark
//                     OS_CONTEXT_STATE_FPSAVED (as __OSSaveFPUContext does);
//   OSLoadContext(c)  c is resumed: reload its FP state if c holds one
//                     (FPSAVED, or OS_CONTEXT_STATE_EXC: the HLE interrupt
//                     entry in ios.cpp saved every register, FPRs included).
// The recompiler emits the calls at the entry of both functions (matched by
// shape). Layout is the one hle_save_context_to_guest/hle_load_context_from_
// guest (ios.cpp) use: fpr[i] at 0x90+8i, FPSCR at 0x194, ps1[i] stored as a
// host double at 0x1C8+8i. NWII_FPU_SWITCH=0 disables it (A/B tests).
inline bool fpu_switch_enabled() {
  static const bool on = [] {
    const char *e = NWII_ENV("NWII_FPU_SWITCH");
    return !(e && e[0] == '0');
  }();
  return on;
}

inline void fpu_save_to_context(CPUContext &ctx, uint32_t c) {
  for (int i = 0; i < 32; i++) {
    uint64_t d;
    std::memcpy(&d, &ctx.fpr[i], 8);
    ctx.mmu.write64(c + 0x90 + i * 8, d);
    std::memcpy(&d, &ctx.ps1[i], 8);
    ctx.mmu.write64(c + 0x1C8 + i * 8, d);
  }
  ctx.mmu.write32(c + 0x190, 0xFFF80000u);  // mffs high word, as stfd stores it
  ctx.mmu.write32(c + 0x194, ctx.fpscr);
  ctx.mmu.write16(c + 0x1A2, ctx.mmu.read16(c + 0x1A2) | 0x0001);
}

inline void fpu_load_from_context(CPUContext &ctx, uint32_t c) {
  for (int i = 0; i < 32; i++) {
    uint64_t d = ctx.mmu.read64(c + 0x90 + i * 8);
    std::memcpy(&ctx.fpr[i], &d, 8);
    d = ctx.mmu.read64(c + 0x1C8 + i * 8);
    std::memcpy(&ctx.ps1[i], &d, 8);
  }
  ctx.fpscr = ctx.mmu.read32(c + 0x194);
}

// Entry of OSSaveContext (r3 = context).
inline void fpu_on_save_context(CPUContext &ctx) {
  const uint32_t c = ctx.gpr[3];
  if (!fpu_switch_enabled() || c < 0x80000000u || c >= 0x94000000u)
    return;
  fpu_save_to_context(ctx, c);
}

// Entry of OSLoadContext (r3 = context), before it clears OS_CONTEXT_STATE_EXC.
inline void fpu_on_load_context(CPUContext &ctx) {
  const uint32_t c = ctx.gpr[3];
  if (!fpu_switch_enabled() || c < 0x80000000u || c >= 0x94000000u)
    return;
  if (ctx.mmu.read16(c + 0x1A2) & 0x0003)
    fpu_load_from_context(ctx, c);
}

// ---- R9 cpucode: inline fast path of psq_l / psq_st -----------------------
// The generated code called the out-of-line CPUContext::psq_load/psq_store for
// every quantized load/store (14 300 sites; matrix and vector code of the SDK
// and the game). perf/apply_r9_psq.py rewrites them to these: when the GQR
// type is float (0, the SDK's GQR0) and every element is in the MEM1 fast
// range of the function's MemView (the same test MemView::read32/write32 use;
// for stores the bound is 0 while a NWII_WATCH is set), the elements are
// loaded/stored inline in the same order with the same conversions
// (load: float widened to double, ps1 = 1.0 if W; store: ConvertToSingleFTZ).
// Anything else (other GQR types, MEM2, MMIO, the gather pipe, unmapped,
// watch) calls the original out-of-line function before touching anything.
#if NWII_FASTMEM && (defined(__x86_64__) || defined(__aarch64__))
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
                                 uint32_t I) {
  const uint32_t paddr = ea & 0x3FFFFFFF;
  if (NWII_LIKELY((ctx.gqr[I] & 0x70000u) == 0 && paddr - 0x10u < mv.lim1 &&
                  (W != 0 || paddr - 0xCu < mv.lim1))) {
    uint32_t v;
    float f;
    std::memcpy(&v, mv.b1 + paddr, 4);
    v = __builtin_bswap32(v);
    std::memcpy(&f, &v, 4);
    const double p0 = f;
    double p1 = 1.0;
    if (W == 0) {
      std::memcpy(&v, mv.b1 + paddr + 4, 4);
      v = __builtin_bswap32(v);
      std::memcpy(&f, &v, 4);
      p1 = f;
    }
    ctx.fpr[D] = p0;
    ctx.ps1[D] = p1;
    return;
  }
  ctx.psq_load(D, ea, W, I);
}
template <uint32_t ID>
NWII_FORCEINLINE void psq_st_fast(CPUContext &ctx, MemView &mv, uint32_t S, uint32_t ea, uint32_t W,
                                  uint32_t I) {
  const uint32_t paddr = ea & 0x3FFFFFFF;
  if (NWII_LIKELY((ctx.gqr[I] & 0x7u) == 0 && paddr - 0x10u < mv.wlim1 &&
                  (W != 0 || paddr - 0xCu < mv.wlim1))) {
    const double p0 = ctx.fpr[S], p1 = ctx.ps1[S];
    uint32_t v = __builtin_bswap32(ppc_single_bits_ftz(p0));
    std::memcpy(mv.b1 + paddr, &v, 4);
    if (W == 0) {
      v = __builtin_bswap32(ppc_single_bits_ftz(p1));
      std::memcpy(mv.b1 + paddr + 4, &v, 4);
    }
    return;
  }
  ctx.psq_store(S, ea, W, I);
}
  ctx.psq_store(S, ea, W, I);
}
#endif
// listed MMIO psq_st sites (gather pipe): straight to the out-of-line path
NWII_FORCEINLINE void psq_st_io(CPUContext &ctx, MemView &, uint32_t S, uint32_t ea, uint32_t W, uint32_t I) {
  ctx.psq_store(S, ea, W, I);
}
NWII_FORCEINLINE void psq_l_io(CPUContext &ctx, MemView &, uint32_t D, uint32_t ea, uint32_t W, uint32_t I) {
  ctx.psq_load(D, ea, W, I);
}

}
}
