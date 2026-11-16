// [r44w] cola local de escrituras al pipe de GX para el renderizador del agua (ver tools/gen_w_agua.py).
// Cada W_PUT* hace lo que hacía mv_.writeN_io(base - 0x8000, v) cuando base es 0xCC010000 (la página del pipe):
// en vez de llamar a MMU::writeN_slow -> GX_WGPIPE_WriteN, guarda los bytes (big endian, como wg_put*) y el tipo
// de la escritura (1 = Write8, 2 = Write16, 4 = WriteF32). W_COMMIT() los entrega en bloque a
// GX_WGPIPE_CommitTape (gx.cpp), que deja el mismo flujo y el mismo estado que las llamadas una a una.
#pragma once
#include <cstdint>
#include <cstring>
namespace nwii { namespace runtime {
void GX_WGPIPE_CommitTape(const uint8_t *bytes, const uint8_t *tape, uint32_t nwrites, uint32_t nbytes);
} }
#define W_QMAX 240u
#define W_DECL uint8_t wq_b[W_QMAX * 4 + 16]; uint8_t wq_t[W_QMAX + 4]; uint32_t wq_nb = 0, wq_nw = 0
#define W_COMMIT() do { if (wq_nw) { nwii::runtime::GX_WGPIPE_CommitTape(wq_b, wq_t, wq_nw, wq_nb); wq_nw = 0; wq_nb = 0; } } while (0)
#define W_FULL() do { if (NWII_UNLIKELY(wq_nw >= W_QMAX)) W_COMMIT(); } while (0)
#define W_PUT8(base, v) do { const uint8_t v_ = (v); \
    if (NWII_LIKELY((base) == 0xCC010000u)) { wq_b[wq_nb] = v_; wq_nb += 1; wq_t[wq_nw++] = 1; W_FULL(); } \
    else { W_COMMIT(); mv_.write8_io((base) + -32768, v_); } } while (0)
#define W_PUT16(base, v) do { const uint16_t v_ = (v); \
    if (NWII_LIKELY((base) == 0xCC010000u)) { const uint16_t be_ = __builtin_bswap16(v_); std::memcpy(wq_b + wq_nb, &be_, 2); \
      wq_nb += 2; wq_t[wq_nw++] = 2; W_FULL(); } \
    else { W_COMMIT(); mv_.write16_io((base) + -32768, v_); } } while (0)
#define W_PUTF32(base, v) do { const float v_ = (v); \
    if (NWII_LIKELY((base) == 0xCC010000u)) { uint32_t u_; std::memcpy(&u_, &v_, 4); u_ = __builtin_bswap32(u_); \
      std::memcpy(wq_b + wq_nb, &u_, 4); wq_nb += 4; wq_t[wq_nw++] = 4; W_FULL(); } \
    else { W_COMMIT(); mv_.write_f32_io((base) + -32768, v_); } } while (0)
