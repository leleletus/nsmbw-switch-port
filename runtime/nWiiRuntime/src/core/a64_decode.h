// R11c fastmem: decoder of the AArch64 load/store forms a fastmem access can fault on.
// Pure function (host-independent), unit-tested on the PC against real devkitA64 output
// (a64_decode_test.cpp + the objdump of a generated TU compiled with the fastmem MemView).
//
// With the recommended design (every guest access is an `asm volatile` with an exception-
// table entry) only the register-offset forms appear:
//   LDRB/LDRH/LDR(W)/LDR(X) Rt, [Xn, Wm, UXTW]   STRB/STRH/STR(W)/STR(X) Rt, [Xn, Wm, UXTW]
// The decoder also accepts the other single-register integer/FP forms GCC emits for plain C
// accesses (unsigned/unscaled/pre/post immediate, sign-extending loads, B/H/S/D/Q registers),
// so a build without asm accessors still decodes; pairs (LDP/STP) and exclusives are refused.
#pragma once
#include <cstdint>

struct A64Access {
  bool ok = false;
  bool store = false;
  bool fp = false;        // Rt is a SIMD&FP register (B/H/S/D/Q)
  bool sign = false;      // sign-extending load (LDRSB/LDRSH/LDRSW)
  bool dst64 = false;     // sign-extend into Xt (else Wt, upper 32 bits zeroed)
  int size = 0;           // bytes accessed
  int rt = 0, rn = 0, rm = -1;
  enum Mode { RegOffset, UImm, Unscaled, PreIdx, PostIdx } mode = RegOffset;
  int extend = 0;         // option field for RegOffset (2 = UXTW, 3 = LSL/UXTX, 6 = SXTW, 7 = SXTX)
  int shift = 0;          // RegOffset: index shifted left by `shift`
  int64_t imm = 0;        // byte offset for the immediate forms
};

inline A64Access a64_decode_ldst(uint32_t w) {
  A64Access a;
  // Load/store register (all single-register forms): bits 29-27 = 111, bit 24..? see ARM ARM C4.1.66
  if (((w >> 27) & 7) != 7 || ((w >> 25) & 1) != 0) return a;  // op0 x x 1 x 0 ...
  const uint32_t size = w >> 30, V = (w >> 26) & 1, opc = (w >> 22) & 3;
  const bool uimm = ((w >> 24) & 3) == 1;
  if (!uimm && ((w >> 24) & 3) != 0) return a;  // pairs etc. (bits 29-28 must be 11 too)
  if (((w >> 28) & 3) != 3) return a;
  a.rt = w & 31;
  a.rn = (w >> 5) & 31;
  a.fp = V;
  if (V) {
    // FP: size:opc -> B(00:0x) H(01:0x) S(10:0x) D(11:0x) Q(00:1x)
    if (opc >= 2) {
      if (size != 0) return a;
      a.size = 16;
      a.store = opc == 2;
    } else {
      a.size = 1 << size;
      a.store = opc == 0;
    }
  } else {
    a.size = 1 << size;
    if (opc == 0) a.store = true;
    else if (opc == 1) a.store = false;
    else {
      // opc 2/3: sign-extending loads (PRFM / invalid for size 3)
      if (size == 3) return a;           // PRFM
      if (size == 2 && opc == 3) return a;
      a.sign = true;
      a.dst64 = opc == 2;
    }
  }
  if (uimm) {
    a.mode = A64Access::UImm;
    a.imm = int64_t((w >> 10) & 0xFFF) * a.size;
  } else {
    const uint32_t bit21 = (w >> 21) & 1, op2 = (w >> 10) & 3;
    const int64_t imm9 = int64_t(int32_t(w << 11) >> 23);
    if (bit21) {
      if (op2 != 2) return a;  // atomics / PAC forms
      a.mode = A64Access::RegOffset;
      a.rm = (w >> 16) & 31;
      a.extend = (w >> 13) & 7;
      if (!(a.extend & 2)) return a;  // UXTB/UXTH etc. unallocated for ld/st
      const int s = (w >> 12) & 1;
      int log2 = 0;
      while ((1 << log2) < a.size) ++log2;
      a.shift = s ? log2 : 0;
    } else {
      a.imm = imm9;
      if (op2 == 0) a.mode = A64Access::Unscaled;
      else if (op2 == 1) a.mode = A64Access::PostIdx;
      else if (op2 == 3) a.mode = A64Access::PreIdx;
      else return a;  // unprivileged LDTR/STTR
    }
  }
  a.ok = true;
  return a;
}

// Effective address from the register file (x[31] = SP for Rn).
inline uint64_t a64_effective_address(const A64Access &a, const uint64_t *x, uint64_t sp) {
  const uint64_t base = a.rn == 31 ? sp : x[a.rn];
  switch (a.mode) {
    case A64Access::RegOffset: {
      uint64_t idx = a.rm == 31 ? 0 : x[a.rm];
      switch (a.extend) {
        case 2: idx = uint32_t(idx); break;                     // UXTW
        case 6: idx = uint64_t(int64_t(int32_t(idx))); break;   // SXTW
        default: break;                                         // LSL / SXTX
      }
      return base + (idx << a.shift);
    }
    case A64Access::PostIdx: return base;
    default: return base + uint64_t(a.imm);
  }
}
