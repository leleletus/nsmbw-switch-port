#pragma once
// [r33-g7] g7.h: caminos rapidos compactos y caminos frios fuera de linea para el codigo generado
// (r9/r33g7/tools/apply_g7.py). Solo lo incluye el codigo generado (output_*.cpp); el runtime no cambia.
// Cada funcion de aqui da EXACTAMENTE el mismo resultado que la de runtime/cpu_context.h que sustituye
// (prueba: r9/r33g7/cputest/g7_unit.cpp, bit a bit, + comparador por funcion + pruebas de CPU).
#include "runtime/cpu_context.h"
#include <cstddef>

// ---- cuenta de instrucciones por bloque (feat "icm") ------------------------------------------
// Sin G7_NOIC: G7IC(n) es el `G5ic += n;` de siempre. Con -DG7_NOIC (solo para la Switch, que va con el reloj
// real: alli ctx.inst_count no interviene en nada salvo el latido y las estadisticas del registro) la suma por
// bloque desaparece (un `add` menos por bloque basico: ~3-4 % de las instrucciones ejecutadas) y el contador
// avanza 32 por sondeo lento, para que siga creciendo si alguien arrancara con el reloj virtual.
#if defined(G7_NOIC)
#define G7IC(n) ((void)0)
#define G7IC_SLOW() (G5ic += 32)
#else
#define G7IC(n) (G5ic += (n))
#define G7IC_SLOW() ((void)0)
#endif

// ---- campos de CR perezosos y empaquetados (feat "cr") ----------------------------------------
// Por campo N (con sus 4 bits como locales en c3) cuatro locales:
//   G5cNm == 0 "modo comparacion": lt/gt/eq = G5cNa </>/== G5cNb (int64: operandos de 32 bits extendidos con
//              signo (cmpw, formas Rc) o sin signo (cmplw)); so = bit 31 de G5cNx (XER al comparar);
//   G5cNm == 1 "modo palabra": G5cNa = los 4 bytes de ctx.cr[N] (lt | gt << 8 | eq << 16 | so << 24, cada uno 0/1).
// Una comparacion solo guarda sus operandos (los bits se calculan donde se usan: el salto que sigue, o los
// volcados, que casi siempre estan en caminos frios); volcar o recargar el campo es UNA palabra de 32 bits.
static_assert(sizeof(nwii::runtime::ConditionField) == 4 && offsetof(nwii::runtime::ConditionField, lt) == 0 &&
              offsetof(nwii::runtime::ConditionField, gt) == 1 && offsetof(nwii::runtime::ConditionField, eq) == 2 &&
              offsetof(nwii::runtime::ConditionField, so) == 3, "g7: disposicion de ConditionField");
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "g7.h (feat cr): solo little-endian"
#endif
#define G7CR_SH_lt 0
#define G7CR_SH_gt 8
#define G7CR_SH_eq 16
#define G7CR_SH_so 24
#define G7CR_lt_(m, a, b, x) ((m) ? (bool)((a) & 0x1) : ((a) < (b)))
#define G7CR_gt_(m, a, b, x) ((m) ? (bool)((a) & 0x100) : ((a) > (b)))
#define G7CR_eq_(m, a, b, x) ((m) ? (bool)((a) & 0x10000) : ((a) == (b)))
#define G7CR_so_(m, a, b, x) ((m) ? (bool)((a) & 0x1000000) : (bool)((x) >> 31))
#define G7CRB(N, bit) G7CR_##bit##_(G5c##N##m, G5c##N##a, G5c##N##b, G5c##N##x)
#define G7CR_RAW(N) (G5c##N##m ? (uint32_t)G5c##N##a \
                               : ((uint32_t)(G5c##N##a < G5c##N##b) | (uint32_t)(G5c##N##a > G5c##N##b) << 8 | \
                                  (uint32_t)(G5c##N##a == G5c##N##b) << 16 | (G5c##N##x >> 31) << 24))
#define G7CR_DECL(N) int64_t G5c##N##a = g7_cr_word(ctx, N), G5c##N##b = 0; uint32_t G5c##N##m = 1, G5c##N##x = 0
#define G7CR_LOAD(N) (G5c##N##m = 1, G5c##N##a = g7_cr_word(ctx, N))
#define G7CR_STORE(N) g7_cr_store(ctx, N, G7CR_RAW(N))
#define G7CR_CMPS(N, X, Y, XER) (G5c##N##m = 0, G5c##N##a = (int64_t)(int32_t)(X), G5c##N##b = (int64_t)(int32_t)(Y), G5c##N##x = (XER))
#define G7CR_CMPU(N, X, Y, XER) (G5c##N##m = 0, G5c##N##a = (int64_t)(uint32_t)(X), G5c##N##b = (int64_t)(uint32_t)(Y), G5c##N##x = (XER))
// forma Rc=1 que no toca so: so se conserva (se guarda como bit 31 de x)
#define G7CR_CMPS3(N, X, Y) do { const uint32_t so_ = G7CRB(N, so); G5c##N##m = 0; G5c##N##a = (int64_t)(int32_t)(X); \
                                 G5c##N##b = (int64_t)(int32_t)(Y); G5c##N##x = so_ << 31; } while (0)
#define G7CR_SET4(N, L, G, E, S) do { const uint32_t r_ = (uint32_t)(bool)(L) | (uint32_t)(bool)(G) << 8 | \
                                      (uint32_t)(bool)(E) << 16 | (uint32_t)(bool)(S) << 24; G5c##N##m = 1; G5c##N##a = r_; } while (0)
#define G7CR_SET(N, bit, E) do { const uint32_t v_ = (bool)(E); const uint32_t r_ = (G7CR_RAW(N) & ~(0xFFu << G7CR_SH_##bit)) | \
                                 (v_ << G7CR_SH_##bit); G5c##N##m = 1; G5c##N##a = r_; } while (0)
NWII_FORCEINLINE uint32_t g7_cr_word(const nwii::runtime::CPUContext &ctx, uint32_t N) { uint32_t w; std::memcpy(&w, &ctx.cr[N], 4); return w; }
NWII_FORCEINLINE void g7_cr_store(nwii::runtime::CPUContext &ctx, uint32_t N, uint32_t w) { std::memcpy(&ctx.cr[N], &w, 4); }

#define G7_COLD __attribute__((noinline, cold))
#define G7_NOINL __attribute__((noinline))
// Caminos frios "sin memoria" (solo dependen de sus argumentos): en AArch64 se llaman por un trampolin en
// ensamblador (g7t_*, definido una vez en la unidad con G7_IMPL) que conserva TODOS los registros salvo el del
// resultado, x16/x17/x30 y los flags. Asi el sitio frio son 2-3 instrucciones (mover el argumento, bl) y GCC no
// tiene que guardar/recargar nada alrededor: una llamada normal de C, aunque sea fria, obliga a salvar los
// registros vivos (los G5 viven en registros caller-saved) y hacia crecer el codigo (+3,9 % en output_10).
// En PC (x86) son llamadas normales a las mismas funciones de C (const: sin efecto sobre la memoria).
#if defined(__aarch64__) && !defined(G7_NO_THUNKS)
#define G7_THUNKS 1
#else
#define G7_THUNKS 0
#endif
extern "C" {
// feat "disp": llamada indirecta (bctrl) con LR = R hecha por el codigo generado; ver G7_IMPL abajo.
uint64_t g7_bctrl_g6(nwii::runtime::CPUContext &ctx, uint64_t ic, uint32_t R, uint32_t pc);
__attribute__((const)) uint32_t g7c_single_bits_g6(double d);
__attribute__((const)) double g7c_force25_g6(double d);
__attribute__((const)) double g7c_madd_tie_g6(double a, double cr, double b_signed, double r);
}

namespace g7 {
using nwii::runtime::CPUContext;
using nwii::runtime::MemView;
using nwii::runtime::MMU;
using nwii::runtime::f32_from;
using nwii::runtime::f64_bits;
using nwii::runtime::f64_from;

// ---- stfs: ConvertToSingle -------------------------------------------------------------------
// ppc_single_bits(d) selecciona bits (no redondea). Si d es exactamente representable en single
// (incluye +-0, +-inf y los denormales de single), esa seleccion de bits ES la conversion IEEE:
//   - normal de single (exp doble 897..1150, 29 bits bajos a cero): mismos signo/exponente/mantisa;
//   - +-0 e +-inf: igual;
//   - denormal de single representable (exp doble 874..896): la rama de denormalizacion trunca, y al
//     ser exacto, truncar = convertir.
// Lo demas (NaN: la comparacion falla; no representable; exp < 874 distinto de cero) va a la funcion
// original, fuera de linea. 5 instrucciones AArch64 en vez de ~10 y sin el bloque frio en cada sitio.
NWII_FORCEINLINE uint32_t single_bits_cold(double d) {
#if G7_THUNKS
  register double a __asm__("d0") = d;
  __asm__("bl g7t_single_g6" : "+w"(a) : : "x16", "x17", "x30", "cc");  // bits del single en los 32 bajos de d0
  return (uint32_t)f64_bits(a);
#else
  return g7c_single_bits_g6(d);
#endif
}
NWII_FORCEINLINE float to_single(double d) {
  const float f = (float)d;
  if (NWII_LIKELY((double)f == d)) return f;
  return f32_from(single_bits_cold(d));
}

// ---- psq_st con GQR de tipo float: ConvertToSingleFTZ ----------------------------------------
// Original: exp > 896 || d == +-0 -> bits seleccionados; si no -> solo el signo. Para +-0 los bits
// seleccionados ya son solo el signo, asi que basta la prueba del exponente.
NWII_FORCEINLINE uint32_t single_bits_ftz(double d) {
  const uint64_t x = f64_bits(d);
  const uint32_t hi = (uint32_t)(x >> 32);
  if (NWII_LIKELY(((hi >> 20) & 0x7ffu) > 896u))
    return (hi & 0xc0000000u) | ((uint32_t)(x >> 29) & 0x3fffffffu);
  return hi & 0x80000000u;
}

// ---- Force25Bit ------------------------------------------------------------------------------
// Con los 28 bits bajos a cero y sin ser denormal de doble, Force25Bit no cambia nada
// (x & 0xFFFFFFFFF8000000) + (x & 0x8000000) == x. Un denormal de doble con los 28 bits bajos a cero
// (exp == 0, frac != 0) va por la rama original. Todo valor single (lfs, resultados de fadds/fmuls...)
// cumple la prueba rapida.
NWII_FORCEINLINE double force25_cold(double d) {
#if G7_THUNKS
  register double a __asm__("d0") = d;
  __asm__("bl g7t_force25_g6" : "+w"(a) : : "x16", "x17", "x30", "cc");
  return a;
#else
  return g7c_force25_g6(d);
#endif
}
NWII_FORCEINLINE double force25(double d) {
  const uint64_t x = f64_bits(d);
  // rapido: 28 bits bajos a cero y (exp != 0 o todo cero). Un solo test: los denormales con los 28 bits
  // bajos a cero son rarisimos, pero deben ir a la rama original.
  if (NWII_LIKELY((x & 0x0FFFFFFFull) == 0 && ((x & 0x7FF0000000000000ull) != 0 || (x & 0x000FFFFFFFFFFFFFull) == 0)))
    return d;
  return force25_cold(d);
}

// ---- multiplicacion-suma single (NI_madd_msub<single>) ---------------------------------------
NWII_FORCEINLINE double madd_tie_cold(double a_, double cr_, double b_signed_, double r_) {
#if G7_THUNKS
  register double a __asm__("d0") = a_;
  register double c __asm__("d1") = cr_;
  register double b __asm__("d2") = b_signed_;
  register double r __asm__("d3") = r_;
  __asm__("bl g7t_madd_tie_g6" : "+w"(a) : "w"(c), "w"(b), "w"(r) : "x16", "x17", "x30", "cc");
  return a;
#else
  return g7c_madd_tie_g6(a_, cr_, b_signed_, r_);
#endif
}
NWII_FORCEINLINE double madd_s(double a, double c, double b_signed) {
  const double cr = force25(c);
  double r = std::fma(a, cr, b_signed);
  if (NWII_UNLIKELY((f64_bits(r) & 0x1FFFFFFFull) == 0x10000000ull)) r = madd_tie_cold(a, cr, b_signed, r);
  return r;
}
NWII_FORCEINLINE double single(double v) { return (double)(float)v; }
NWII_FORCEINLINE double neg_keep_nan(double v) { return std::isnan(v) ? v : -v; }

// Escalares single (mismas expresiones que CPUContext::fmuls & cia, con force25/madd_s de arriba).
NWII_FORCEINLINE void fmuls(CPUContext &c, uint32_t D, uint32_t A, uint32_t C) { c.fill(D, single(c.fpr[A] * force25(c.fpr[C]))); }
NWII_FORCEINLINE void fmadds(CPUContext &c, uint32_t D, uint32_t A, uint32_t C, uint32_t B) { c.fill(D, single(madd_s(c.fpr[A], c.fpr[C], c.fpr[B]))); }
NWII_FORCEINLINE void fmsubs(CPUContext &c, uint32_t D, uint32_t A, uint32_t C, uint32_t B) { c.fill(D, single(madd_s(c.fpr[A], c.fpr[C], -c.fpr[B]))); }
NWII_FORCEINLINE void fnmadds(CPUContext &c, uint32_t D, uint32_t A, uint32_t C, uint32_t B) { c.fill(D, neg_keep_nan(single(madd_s(c.fpr[A], c.fpr[C], c.fpr[B])))); }
NWII_FORCEINLINE void fnmsubs(CPUContext &c, uint32_t D, uint32_t A, uint32_t C, uint32_t B) { c.fill(D, neg_keep_nan(single(madd_s(c.fpr[A], c.fpr[C], -c.fpr[B])))); }
// Pares. Los de multiplicacion-suma van fuera de linea, una copia compartida (en c3 GCC ya los dejaba fuera
// de linea casi siempre; en linea hacen crecer las funciones de matrices un 30-50 %).
NWII_FORCEINLINE void ps_mul(CPUContext &c, uint32_t D, uint32_t A, uint32_t C) {
  c.set_ps(D, single(c.fpr[A] * force25(c.fpr[C])), single(c.ps1[A] * force25(c.ps1[C])));
}
NWII_FORCEINLINE void ps_muls0(CPUContext &c, uint32_t D, uint32_t A, uint32_t C) {
  const double c0 = force25(c.fpr[C]);
  c.set_ps(D, single(c.fpr[A] * c0), single(c.ps1[A] * c0));
}
NWII_FORCEINLINE void ps_muls1(CPUContext &c, uint32_t D, uint32_t A, uint32_t C) {
  const double c1 = force25(c.ps1[C]);
  c.set_ps(D, single(c.fpr[A] * c1), single(c.ps1[A] * c1));
}
// [relevo] -DG7_INL_MADDS: ps_madds0/ps_madds1 (las mas llamadas, 2/3 del tiempo de estas funciones) en linea
#ifdef G7_INL_MADDS
#define G7_MADDS_INL NWII_FORCEINLINE
#else
#define G7_MADDS_INL G7_NOINL inline
#endif
G7_NOINL inline void ps_madd(CPUContext &c, uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
  c.set_ps(D, single(madd_s(c.fpr[A], c.fpr[C], c.fpr[B])), single(madd_s(c.ps1[A], c.ps1[C], c.ps1[B])));
}
G7_NOINL inline void ps_msub(CPUContext &c, uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
  c.set_ps(D, single(madd_s(c.fpr[A], c.fpr[C], -c.fpr[B])), single(madd_s(c.ps1[A], c.ps1[C], -c.ps1[B])));
}
G7_NOINL inline void ps_nmadd(CPUContext &c, uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
  c.set_ps(D, neg_keep_nan(single(madd_s(c.fpr[A], c.fpr[C], c.fpr[B]))),
           neg_keep_nan(single(madd_s(c.ps1[A], c.ps1[C], c.ps1[B]))));
}
G7_NOINL inline void ps_nmsub(CPUContext &c, uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
  c.set_ps(D, neg_keep_nan(single(madd_s(c.fpr[A], c.fpr[C], -c.fpr[B]))),
           neg_keep_nan(single(madd_s(c.ps1[A], c.ps1[C], -c.ps1[B]))));
}
G7_MADDS_INL void ps_madds0(CPUContext &c, uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
  const double c0 = c.fpr[C];
  c.set_ps(D, single(madd_s(c.fpr[A], c0, c.fpr[B])), single(madd_s(c.ps1[A], c0, c.ps1[B])));
}
G7_MADDS_INL void ps_madds1(CPUContext &c, uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
  const double c1 = c.ps1[C];
  c.set_ps(D, single(madd_s(c.fpr[A], c1, c.fpr[B])), single(madd_s(c.ps1[A], c1, c.ps1[B])));
}

// ---- psq_l / psq_st --------------------------------------------------------------------------
// Camino general (GQR no float): una sola funcion compartida fuera de linea, sin lambdas; hace lo
// mismo que CPUContext::psq_load / psq_store (mismos accesos por ctx.mmu, mismo orden, mismas
// conversiones). dwi = D | W << 8 | I << 16 (constantes del sitio: un solo argumento).
G7_NOINL inline void psq_l_gen(CPUContext &ctx, uint32_t ea, uint32_t dwi) {
  const uint32_t D = dwi & 0xFF, W = (dwi >> 8) & 0xFF, I = dwi >> 16;
  const uint32_t g = ctx.gqr[I];
  const uint32_t t = (g >> 16) & 0x7;
  const float scale = (float)nwii::runtime::gqr_pow2(-nwii::runtime::gqr_scale(g >> 24));
  MMU &m = ctx.mmu;
  double p0, p1 = 1.0;
  switch (t) {
  case 0:
    p0 = m.read_f32(ea);
    if (W == 0) p1 = m.read_f32(ea + 4);
    break;
  case 4:
    p0 = (float)m.read8(ea) * scale;
    if (W == 0) p1 = (float)m.read8(ea + 1) * scale;
    break;
  case 5:
    p0 = (float)m.read16(ea) * scale;
    if (W == 0) p1 = (float)m.read16(ea + 2) * scale;
    break;
  case 6:
    p0 = (float)(int8_t)m.read8(ea) * scale;
    if (W == 0) p1 = (float)(int8_t)m.read8(ea + 1) * scale;
    break;
  case 7:
    p0 = (float)(int16_t)m.read16(ea) * scale;
    if (W == 0) p1 = (float)(int16_t)m.read16(ea + 2) * scale;
    break;
  default:
    p0 = 0.0;
    if (W == 0) p1 = 0.0;
    break;
  }
  ctx.fpr[D] = p0;
  ctx.ps1[D] = p1;
}
G7_NOINL inline void psq_st_gen(CPUContext &ctx, uint32_t ea, uint32_t swi) {
  const uint32_t S = swi & 0xFF, W = (swi >> 8) & 0xFF, I = swi >> 16;
  const uint32_t g = ctx.gqr[I];
  const uint32_t t = g & 0x7;
  const double scale = nwii::runtime::gqr_pow2(nwii::runtime::gqr_scale(g >> 8));
  MMU &m = ctx.mmu;
  const double p0 = ctx.fpr[S], p1 = ctx.ps1[S];
  using nwii::runtime::ppc_quantize;
  switch (t) {
  case 0:
    m.write_f32(ea, f32_from(nwii::runtime::ppc_single_bits_ftz(p0)));
    if (W == 0) m.write_f32(ea + 4, f32_from(nwii::runtime::ppc_single_bits_ftz(p1)));
    break;
  case 4:
    m.write8(ea, (uint8_t)ppc_quantize(p0, scale, 0, 255));
    if (W == 0) m.write8(ea + 1, (uint8_t)ppc_quantize(p1, scale, 0, 255));
    break;
  case 5:
    m.write16(ea, (uint16_t)ppc_quantize(p0, scale, 0, 65535));
    if (W == 0) m.write16(ea + 2, (uint16_t)ppc_quantize(p1, scale, 0, 65535));
    break;
  case 6:
    m.write8(ea, (uint8_t)(int8_t)ppc_quantize(p0, scale, -128, 127));
    if (W == 0) m.write8(ea + 1, (uint8_t)(int8_t)ppc_quantize(p1, scale, -128, 127));
    break;
  case 7:
    m.write16(ea, (uint16_t)(int16_t)ppc_quantize(p0, scale, -32768, 32767));
    if (W == 0) m.write16(ea + 2, (uint16_t)(int16_t)ppc_quantize(p1, scale, -32768, 32767));
    break;
  default:
    break;  // tipos 1-3: psq_store no escribe nada
  }
}

#if NWII_FASTMEM && (defined(__x86_64__) || defined(__aarch64__))
template <uint32_t ID>
NWII_FORCEINLINE void psq_l(CPUContext &ctx, MemView &mv, uint32_t D, uint32_t ea, uint32_t W, uint32_t I) {
  if (NWII_LIKELY((ctx.gqr[I] & 0x70000u) == 0)) {
    const double p0 = mv.read_f32<ID>(ea);
    const double p1 = W == 0 ? (double)mv.read_f32<ID>(ea + 4) : 1.0;
    ctx.fpr[D] = p0;
    ctx.ps1[D] = p1;
    return;
  }
  psq_l_gen(ctx, ea, D | (W << 8) | (I << 16));
}
template <uint32_t ID>
NWII_FORCEINLINE void psq_st(CPUContext &ctx, MemView &mv, uint32_t S, uint32_t ea, uint32_t W, uint32_t I) {
  if (NWII_LIKELY((ctx.gqr[I] & 0x7u) == 0)) {
    const double p0 = ctx.fpr[S], p1 = ctx.ps1[S];
    mv.write_f32<ID>(ea, f32_from(single_bits_ftz(p0)));
    if (W == 0) mv.write_f32<ID>(ea + 4, f32_from(single_bits_ftz(p1)));
    return;
  }
  psq_st_gen(ctx, ea, S | (W << 8) | (I << 16));
}
#else
// Sin fastmem (no se usa en los binarios de hoy): las originales.
template <uint32_t ID>
NWII_FORCEINLINE void psq_l(CPUContext &ctx, MemView &mv, uint32_t D, uint32_t ea, uint32_t W, uint32_t I) {
  nwii::runtime::psq_l_fast<ID>(ctx, mv, D, ea, W, I);
}
template <uint32_t ID>
NWII_FORCEINLINE void psq_st(CPUContext &ctx, MemView &mv, uint32_t S, uint32_t ea, uint32_t W, uint32_t I) {
  nwii::runtime::psq_st_fast<ID>(ctx, mv, S, ea, W, I);
}
#endif
// sitios MMIO listados (tuberia de GX)
NWII_FORCEINLINE void psq_st_io(CPUContext &ctx, MemView &, uint32_t S, uint32_t ea, uint32_t W, uint32_t I) {
  psq_st_gen(ctx, ea, S | (W << 8) | (I << 16));
}
NWII_FORCEINLINE void psq_l_io(CPUContext &ctx, MemView &, uint32_t D, uint32_t ea, uint32_t W, uint32_t I) {
  psq_l_gen(ctx, ea, D | (W << 8) | (I << 16));
}

// ---- escrituras en sitios MMIO listados (casi todas: tuberia write-gather de GX, 0xCC008000) ----
// MMU::writeN_slow hace: (watch) -> si paddr esta en la pagina 0x0C008000 -> GX_WGPIPE_WriteN.
// Aqui se llama a GX_WGPIPE_WriteN directamente en ese caso (sin watch: mv.watch es g_watch_addr != 0,
// fijado al arrancar); lo demas, a la funcion lenta de siempre. Una llamada menos por escritura.
NWII_FORCEINLINE bool is_wgpipe(const MemView &mv, uint32_t ea) {
  return NWII_LIKELY(!mv.watch && (ea & 0x3FFFF000u) == 0x0C008000u);
}
NWII_FORCEINLINE void write8_io(MemView &mv, uint32_t ea, uint8_t v) {
  if (is_wgpipe(mv, ea)) nwii::runtime::GX_WGPIPE_Write8(v); else mv.m.write8_slow(ea, v);
}
NWII_FORCEINLINE void write16_io(MemView &mv, uint32_t ea, uint16_t v) {
  if (is_wgpipe(mv, ea)) nwii::runtime::GX_WGPIPE_Write16(v); else mv.m.write16_slow(ea, v);
}
NWII_FORCEINLINE void write32_io(MemView &mv, uint32_t ea, uint32_t v) {
  if (is_wgpipe(mv, ea)) nwii::runtime::GX_WGPIPE_Write32(v); else mv.m.write32_slow(ea, v);
}
NWII_FORCEINLINE void write_f32_io(MemView &mv, uint32_t ea, float v) {
  if (is_wgpipe(mv, ea)) nwii::runtime::GX_WGPIPE_WriteF32(v); else mv.m.write_f32_slow(ea, v);
}

// ---- sondeo de interrupciones compacto (feat "poll") -----------------------------------------
// Solo la cuenta atras (ldr, subs, str, b.le). NECESITA el runtime de r9/r33g7 (o cualquiera con sus
// avisos): alli todo lo que antes se miraba aqui en cada sondeo -- MSR[EE] && (poll_dec_due |
// PI_INTSR & PI_INTMR | pending_cb_count) -- fuerza el sondeo lento siguiente con poll_kick
// (nWiiRuntime/src/hle/ios.cpp, "[r33-g7] sondeo compacto"). Con un runtime sin esos avisos una
// interrupcion esperaria hasta el siguiente sondeo lento natural (cada NWII_POLL_EVERY sondeos).
NWII_FORCEINLINE bool poll_due(CPUContext &ctx) {
#if defined(__aarch64__) && !defined(G7_NO_ASM_POLL)
  // GCC no funde la resta con la comparacion (sub; str; cmp; b.le): a mano son 4 instrucciones.
  // `subs` deja los flags de (antes - 1) frente a 0, o sea antes <= 1: lo mismo que `left <= 0`.
  __asm__ goto("ldr w16, %[m]\n\tsubs w16, w16, #1\n\tstr w16, %[m]\n\tb.le %l[due]"
               : [m] "+m"(*(int32_t *)&ctx.poll_left) : : "x16", "cc" : due);
  return false;
due: __attribute__((cold));
  return true;
#else
  const int32_t left = ctx.poll_left.load(std::memory_order_relaxed) - 1;
  ctx.poll_left.store(left, std::memory_order_relaxed);
  return NWII_UNLIKELY(left <= 0);
#endif
}

}  // namespace g7

// ================================================================================================
// G7_IMPL: definiciones unicas (una sola unidad: output_0.cpp): funciones frias de C, trampolines y autoprueba.
#ifdef G7_IMPL
#include <cstdio>
#include <cstdlib>
extern "C" {
uint32_t g7c_single_bits_g6(double d) { return nwii::runtime::ppc_single_bits(d); }
double g7c_force25_g6(double d) { return nwii::runtime::ppc_force25(d); }
double g7c_madd_tie_g6(double a, double cr, double b_signed, double r) {
  // la rama de empate de ppc_madd_s (NI_madd_msub<single>), tal cual
  const uint64_t bits = nwii::runtime::f64_bits(r);
  const double a_prime = b_signed - r;
  const double b_prime = r + a_prime;
  const double delta_a = std::fma(a, cr, a_prime);
  const double delta_b = b_signed - b_prime;
  const double error = delta_a + delta_b;
  if (error != 0.0) r = nwii::runtime::f64_from(((error > 0.0) == (r > 0.0)) ? bits + 1 : bits - 1);
  return r;
}
}
// ---- feat "disp": bctrl -------------------------------------------------------------------------
// Tabla hash (direccion invitada de entrada -> cuerpo _g6), montada la primera vez a partir de g7_disp.inc
// (todas las funciones _g6) y SOLO con las entradas para las que la tabla de despacho de verdad
// (g_disp_table, fastdispatch.cpp) lleva a esa misma funcion: asi el destino es el que elegiria nwii_dispatch.
#if __has_include("g7_disp.inc")
namespace g7 {
typedef uint64_t (*body_fn)(nwii::runtime::CPUContext &, uint64_t, uint64_t);
struct DispEnt { uint32_t addr; uint32_t pad; body_fn fn; };
struct DispSrc { uint32_t addr; nwii_fn wrap; body_fn body; };
static constexpr uint32_t kDispMask = 0x1FFFF;  // 131 072 casillas para ~38 700 entradas
static DispEnt *g_disp_hash = nullptr;
#define G7D(a, n) {a, n, n##_g6},
static const DispSrc kDispSrc[] = {
#include "g7_disp.inc"
};
#undef G7D
__attribute__((noinline, cold)) static DispEnt *disp_init() {
  if (!g_disp_table) return nullptr;  // NWII_SLOW_DISPATCH (o el comparador sin --dispatch): nwii_dispatch tampoco hace nada
  DispEnt *t = new DispEnt[kDispMask + 1]();
  for (const DispSrc &e : kDispSrc) {
    const uint32_t off = e.addr - g_disp_base;
    if ((off & 3) || off >= g_disp_span || g_disp_table[off >> 2] != e.wrap) continue;
    uint32_t i = (e.addr >> 2) & kDispMask;
    while (t[i].addr) i = (i + 1) & kDispMask;
    t[i].addr = e.addr;
    t[i].fn = e.body;
  }
  g_disp_hash = t;
  return t;
}
}  // namespace g7
extern "C" uint64_t g7_bctrl_g6(nwii::runtime::CPUContext &ctx, uint64_t ic, uint32_t R, uint32_t pc) {
  const uint32_t ctr = ctx.ctr;
  const g7::DispEnt *t = g7::g_disp_hash;
  if (NWII_UNLIKELY(!t)) t = g7::disp_init();
  if (NWII_LIKELY(t != nullptr && ctr != 0)) {  // [relevo 23:35] ctr == 0: una casilla vacia (addr 0) casaba -> puntero nulo; va a la sentencia de c3
    for (uint32_t i = (ctr >> 2) & g7::kDispMask;; i = (i + 1) & g7::kDispMask) {
      const g7::DispEnt &e = t[i];
      if (NWII_LIKELY(e.addr == ctr)) {
        if (NWII_UNLIKELY(!nwii::runtime::native_stack_ok())) break;  // sin pila nativa: por nwii_dispatch (vuelve a run_game)
        // Llamada directa: el cuerpo entra por su direccion con G5lr = R; ctx.pc/lr/inst_count no hacen falta
        // (protocolo 3 de c3: son locales del llamado, que los escribe antes de cualquier escape).
        const uint64_t r = e.fn(ctx, ic, R);
        if (NWII_UNLIKELY((int64_t)r < 0)) return r;  // ctx exacto y pc/lr != R: el llamador desenrolla (g6_exact)
        // retorno normal a R: lo que dejaba g6_slowret con el envoltorio (el codigo que sigue lo da por escrito)
        ctx.inst_count = r;
        ctx.pc = R;
        ctx.lr = R;
        return r;
      }
      if (!e.addr) break;
    }
  }
  // La sentencia de c3, tal cual (destino que no es la entrada de una funcion _g6, ctr nulo, sin tabla, sin pila).
  ctx.lr = R;
  ctx.pc = pc;
  ctx.inst_count = ic;
  if (ctx.ctr) {
    ctx.pc = ctx.ctr;
    nwii_dispatch(ctx);
    if (ctx.pc != R) return G6BIT;
  } else {
    nwii::runtime::note_null_call(R - 4, ctx.lr);
  }
  return ctx.inst_count & ~G6BIT;
}
#endif  // g7_disp.inc

#if G7_THUNKS
// Trampolin: guarda x0-x18 y q0-q31, llama a la funcion de C y restaura todo menos el resultado (d0).
// res=1: la funcion de C devuelve w0 y el trampolin lo deja en s0 (d0 = bits extendidos con ceros).
__asm__(R"(
        .macro G7T name, cfn, res
        .pushsection .text.g7_thunks,"ax",%progbits
        .balign 16
        .global \name
        .hidden \name
        .type \name, %function
\name:
        sub     sp, sp, #688
        stp     x29, x30, [sp]
        mov     x29, sp
        stp     x0, x1, [sp, #16]
        stp     x2, x3, [sp, #32]
        stp     x4, x5, [sp, #48]
        stp     x6, x7, [sp, #64]
        stp     x8, x9, [sp, #80]
        stp     x10, x11, [sp, #96]
        stp     x12, x13, [sp, #112]
        stp     x14, x15, [sp, #128]
        stp     x16, x17, [sp, #144]
        str     x18, [sp, #160]
        stp     q0, q1, [sp, #176]
        stp     q2, q3, [sp, #208]
        stp     q4, q5, [sp, #240]
        stp     q6, q7, [sp, #272]
        stp     q8, q9, [sp, #304]
        stp     q10, q11, [sp, #336]
        stp     q12, q13, [sp, #368]
        stp     q14, q15, [sp, #400]
        stp     q16, q17, [sp, #432]
        stp     q18, q19, [sp, #464]
        stp     q20, q21, [sp, #496]
        stp     q22, q23, [sp, #528]
        stp     q24, q25, [sp, #560]
        stp     q26, q27, [sp, #592]
        stp     q28, q29, [sp, #624]
        stp     q30, q31, [sp, #656]
        bl      \cfn
        .if \res
        fmov    s0, w0
        .endif
        ldr     q1, [sp, #192]
        ldp     q2, q3, [sp, #208]
        ldp     q4, q5, [sp, #240]
        ldp     q6, q7, [sp, #272]
        ldp     q8, q9, [sp, #304]
        ldp     q10, q11, [sp, #336]
        ldp     q12, q13, [sp, #368]
        ldp     q14, q15, [sp, #400]
        ldp     q16, q17, [sp, #432]
        ldp     q18, q19, [sp, #464]
        ldp     q20, q21, [sp, #496]
        ldp     q22, q23, [sp, #528]
        ldp     q24, q25, [sp, #560]
        ldp     q26, q27, [sp, #592]
        ldp     q28, q29, [sp, #624]
        ldp     q30, q31, [sp, #656]
        ldp     x0, x1, [sp, #16]
        ldp     x2, x3, [sp, #32]
        ldp     x4, x5, [sp, #48]
        ldp     x6, x7, [sp, #64]
        ldp     x8, x9, [sp, #80]
        ldp     x10, x11, [sp, #96]
        ldp     x12, x13, [sp, #112]
        ldp     x14, x15, [sp, #128]
        ldp     x16, x17, [sp, #144]
        ldr     x18, [sp, #160]
        ldp     x29, x30, [sp]
        add     sp, sp, #688
        ret
        .size \name, .-\name
        .popsection
        .endm
        G7T g7t_single_g6, g7c_single_bits_g6, 1
        G7T g7t_force25_g6, g7c_force25_g6, 0
        G7T g7t_madd_tie_g6, g7c_madd_tie_g6, 0

        // uint64_t g7t_regcheck_g6(void *thunk, const double in[4], uint64_t *out_d0)
        // Llama a thunk con d0-d3 = in y un patron en x0-x15 y en v4-v31 (las dos mitades) y v1-v3;
        // devuelve cuantos registros cambiaron (0 = el trampolin conserva todo); *out_d0 = bits de d0.
        .pushsection .text.g7_thunks,"ax",%progbits
        .balign 16
        .global g7t_regcheck_g6
        .hidden g7t_regcheck_g6
        .type g7t_regcheck_g6, %function
g7t_regcheck_g6:
        stp     x29, x30, [sp, #-144]!
        mov     x29, sp
        stp     x19, x20, [sp, #16]
        stp     x21, x22, [sp, #32]
        stp     x23, x24, [sp, #48]
        stp     d8, d9, [sp, #64]
        stp     d10, d11, [sp, #80]
        stp     d12, d13, [sp, #96]
        stp     d14, d15, [sp, #112]
        mov     x19, x0
        mov     x21, x1
        mov     x20, x2
        mov     x23, #0
        .irp n,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
        mov     x22, #\n
        movk    x22, #0x5A5A, lsl #32
        movk    x22, #0xC3C3, lsl #48
        fmov    d\n, x22
        mov     v\n\().d[1], x22
        .endr
        ldp     d0, d1, [x21]
        ldp     d2, d3, [x21, #16]
        .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        mov     x\n, #\n
        movk    x\n, #0xA5A5, lsl #48
        .endr
        blr     x19
        .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15
        mov     x22, #\n
        movk    x22, #0xA5A5, lsl #48
        cmp     x\n, x22
        cinc    x23, x23, ne
        .endr
        .irp n,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
        mov     x22, #\n
        movk    x22, #0x5A5A, lsl #32
        movk    x22, #0xC3C3, lsl #48
        fmov    x24, d\n
        cmp     x24, x22
        cinc    x23, x23, ne
        mov     x24, v\n\().d[1]
        cmp     x24, x22
        cinc    x23, x23, ne
        .endr
        ldr     x22, [x21, #8]
        fmov    x24, d1
        cmp     x24, x22
        cinc    x23, x23, ne
        ldr     x22, [x21, #16]
        fmov    x24, d2
        cmp     x24, x22
        cinc    x23, x23, ne
        ldr     x22, [x21, #24]
        fmov    x24, d3
        cmp     x24, x22
        cinc    x23, x23, ne
        str     d0, [x20]
        mov     x0, x23
        ldp     x19, x20, [sp, #16]
        ldp     x21, x22, [sp, #32]
        ldp     x23, x24, [sp, #48]
        ldp     d8, d9, [sp, #64]
        ldp     d10, d11, [sp, #80]
        ldp     d12, d13, [sp, #96]
        ldp     d14, d15, [sp, #112]
        ldp     x29, x30, [sp], #144
        ret
        .size g7t_regcheck_g6, .-g7t_regcheck_g6
        .popsection
)");
extern "C" {
void g7t_single_g6();
void g7t_force25_g6();
void g7t_madd_tie_g6();
uint64_t g7t_regcheck_g6(void (*thunk)(), const double *in4, uint64_t *out_d0);
}
#endif  // G7_THUNKS

namespace g7 {
// Autoprueba al arrancar (constructor estatico): en la consola es la unica maquina donde corren los
// trampolines, asi que se prueban alli mismo antes de ejecutar nada del juego: resultado identico a la
// funcion de C y todos los registros conservados. Si falla: mensaje y abort() (la NRO no arranca).
// Tambien comprueba, en cualquier maquina, los caminos rapidos contra los originales con valores frios.
__attribute__((noinline)) inline int selftest() {
  using namespace nwii::runtime;
  static const uint64_t vals[] = {
      0x0000000000000000ull, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000001ull,
      0x36A0000000000000ull, 0x36B0000000000000ull, 0x3690000000000000ull, 0x380FFFFFFFFFFFFFull,
      0x3800000000000000ull, 0xB7F0000000000000ull, 0x37A123456789ABCDull, 0x369FFFFFFFFFFFFFull,
      0x7FF0000000000000ull, 0xFFF0000000000000ull, 0x7FF8000000000001ull, 0x7FF4000000000000ull,
      0xFFF123456789ABCDull, 0x47EFFFFFE0000000ull, 0x47EFFFFFF0000000ull, 0x47F0000000000000ull,
      0x0000000000000001ull, 0x800FFFFFFFFFFFFFull, 0x0000000010000000ull, 0x0008000000000000ull,
      0x3FF0000008000000ull, 0x3FF000000FFFFFFFull, 0x3FF0000010000000ull, 0x400921FB54442D18ull,
      0xC05EDCCCCCCCCCCDull, 0x3E112E0BE826D695ull, 0x0010000000000000ull, 0x001FFFFFF8000000ull};
  const int n = (int)(sizeof(vals) / sizeof(vals[0]));
  int bad = 0;
  for (int i = 0; i < n; i++) {
    const double d = f64_from(vals[i]);
    uint32_t a, b;
    const float f = to_single(d), g = ppc_to_single(d);
    std::memcpy(&a, &f, 4); std::memcpy(&b, &g, 4);
    bad += a != b;
    bad += single_bits_cold(d) != ppc_single_bits(d);
    bad += single_bits_ftz(d) != ppc_single_bits_ftz(d);
    bad += f64_bits(force25(d)) != f64_bits(ppc_force25(d));
    bad += f64_bits(force25_cold(d)) != f64_bits(ppc_force25(d));
    for (int j = 0; j < n; j += 3) {
      const double c = f64_from(vals[j]), e = f64_from(vals[(i + j) % n]);
      const double r1 = madd_s(d, c, e), r2 = ppc_madd_s(d, c, e);
      bad += f64_bits(r1) != f64_bits(r2) && !(std::isnan(r1) && std::isnan(r2));
      const double t1 = madd_tie_cold(d, c, e, d), t2 = g7c_madd_tie_g6(d, c, e, d);
      bad += f64_bits(t1) != f64_bits(t2) && !(std::isnan(t1) && std::isnan(t2));
    }
  }
  // el caso de empate confirmado en hardware (fmadds de Mario Strikers, de las pruebas de CPU)
  {
    const double a = (double)f32_from(0x42480000u), c = (double)f32_from(0xbc88cc38u), b = (double)f32_from(0x1b1c72a0u);
    bad += f64_bits(madd_s(a, c, b)) != f64_bits(ppc_madd_s(a, c, b));
  }
#if G7_THUNKS
  uint64_t regs = 0;
  for (int i = 0; i < n; i++) {
    const double in[4] = {f64_from(vals[i]), f64_from(vals[(i + 5) % n]), f64_from(vals[(i + 11) % n]), f64_from(vals[(i + 17) % n])};
    uint64_t out = 0;
    regs += g7t_regcheck_g6(g7t_single_g6, in, &out);
    bad += out != (uint64_t)ppc_single_bits(in[0]);
    regs += g7t_regcheck_g6(g7t_force25_g6, in, &out);
    bad += out != f64_bits(ppc_force25(in[0]));
    regs += g7t_regcheck_g6(g7t_madd_tie_g6, in, &out);
    const double w = g7c_madd_tie_g6(in[0], in[1], in[2], in[3]);
    bad += out != f64_bits(w) && !std::isnan(w);
  }
  if (regs) bad += 1000000;
#endif
  if (bad) {
    std::printf("[G7] AUTOPRUEBA FALLIDA (%d): los ayudantes de g7.h no dan lo mismo que los originales\n", bad);
    std::fflush(stdout);
    std::abort();
  }
  std::printf("[G7] autoprueba ok (trampolines: %d)\n", (int)G7_THUNKS);
  return 0;
}
static const int g_selftest_done __attribute__((used)) = selftest();
}  // namespace g7
#endif  // G7_IMPL
