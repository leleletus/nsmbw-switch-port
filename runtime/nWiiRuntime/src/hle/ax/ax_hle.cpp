#include "ax_hle.h"

#include "runtime/cpu_context.h"
#include "runtime/nwii_env.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace nwii::runtime {
extern MMU *g_mmu;
}

// R8: same algorithm as the r7b scalar mixer (ax_hle_ref.cpp), bit for bit.
// What changed is how it touches memory and the volume loops:
//  - guest memory is read and written in blocks through host pointers (one
//    bounds check per block instead of one per 16-bit word); a block that is
//    not contiguous host memory falls back to the per-word path;
//  - the DSP accelerator reads sample bytes straight from MEM1/MEM2;
//  - a voice block is written back only where it changed, and not at all when
//    the voice is stopped (the old code rewrote all 0x12C bytes of every PB);
//    same memory contents afterwards, and a voice the guest edits while the
//    AX thread runs is never overwritten with a stale copy;
//  - envelope and bus mixing (per-sample volume ramp, clamp, accumulate) use
//    8-lane GCC vectors (SSE2 on PC, NEON on the Switch). Every product fits
//    in 32 bits (int16 x uint16), so the int32 lanes equal the int64 math.

namespace nwii::runtime::ax {

RangeLog *g_write_log = nullptr;
RangeLog *g_read_log = nullptr;

namespace {

constexpr int kFrame = 96;     // samples per 3 ms frame at 32 kHz
constexpr int kWmFrame = 18;   // Wii Remote speaker samples per frame
constexpr int kMaxVoices = 512;

typedef int32_t v8si __attribute__((vector_size(32)));
typedef int16_t v8hi __attribute__((vector_size(16)));
static_assert(kFrame % 8 == 0);

// ---- guest memory (big-endian, physical or cached addresses) ----

uint8_t *mem(uint32_t addr) { return g_mmu ? g_mmu->get_ptr(addr) : nullptr; }

// Host pointer for [addr, addr + bytes) if it is one contiguous host block
// (what the per-word path would touch), else nullptr.
uint8_t *span(uint32_t addr, uint32_t bytes) {
  uint8_t *p = mem(addr);
  if (!p) return nullptr;
  uint8_t *q = mem(addr + bytes - 1);
  return q == p + (bytes - 1) ? p : nullptr;
}

inline void log_w(uint8_t *p, uint32_t n) {
  if (__builtin_expect(g_write_log != nullptr, 0)) g_write_log->push_back({p, n});
}
inline void log_r(uint8_t *p, uint32_t n) {
  if (__builtin_expect(g_read_log != nullptr, 0)) g_read_log->push_back({p, n});
}

uint16_t rd16(uint32_t a) {
  uint8_t *p = mem(a);
  if (!p) return 0;
  log_r(p, 2);
  return (uint16_t)((p[0] << 8) | p[1]);
}

void wr16(uint32_t a, uint16_t v) {
  if (uint8_t *p = mem(a)) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    log_w(p, 2);
  }
}

int32_t rd32(uint32_t a) { return (int32_t)(((uint32_t)rd16(a) << 16) | rd16(a + 2)); }

void wr32(uint32_t a, int32_t v) {
  wr16(a, (uint16_t)((uint32_t)v >> 16));
  wr16(a + 2, (uint16_t)v);
}

inline uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)(v >> 8);
  p[1] = (uint8_t)v;
}

void load16(uint32_t addr, uint16_t *dst, int n) {
  if (uint8_t *p = span(addr, n * 2)) {
    log_r(p, n * 2);
    for (int i = 0; i < n; ++i) dst[i] = be16(p + i * 2);
  } else {
    for (int i = 0; i < n; ++i) dst[i] = rd16(addr + i * 2);
  }
}

void load32(uint32_t addr, int32_t *dst, int n) {
  if (uint8_t *p = span(addr, n * 4)) {
    log_r(p, n * 4);
    for (int i = 0; i < n; ++i) {
      uint32_t v;
      std::memcpy(&v, p + i * 4, 4);
      dst[i] = (int32_t)__builtin_bswap32(v);
    }
  } else {
    for (int i = 0; i < n; ++i) dst[i] = rd32(addr + i * 4);
  }
}

void store32(uint32_t addr, const int32_t *src, int n) {
  if (uint8_t *p = span(addr, n * 4)) {
    for (int i = 0; i < n; ++i) {
      const uint32_t v = __builtin_bswap32((uint32_t)src[i]);
      std::memcpy(p + i * 4, &v, 4);
    }
    log_w(p, n * 4);
  } else {
    for (int i = 0; i < n; ++i) wr32(addr + i * 4, src[i]);
  }
}

// The DSP accelerator sees "ARAM" addresses; on Wii those are physical RAM
// addresses, MEM2 when bit 28 is set. Same result as get_ptr() on
// 0x10000000|(a & 0x03FFFFFF) (always inside MEM2) or a & 0x01FFFFFF (MEM1
// below 24 MiB, else unmapped = 0).
uint8_t *g_m1 = nullptr, *g_m2 = nullptr;
constexpr uint32_t kMem1Bytes = 24u * 1024 * 1024;

inline uint8_t aram8(uint32_t a) {
  if (a & 0x10000000) {
    if (__builtin_expect(g_read_log != nullptr, 0)) log_r(g_m2 + (a & 0x03FFFFFF), 1);
    return g_m2[a & 0x03FFFFFF];
  }
  const uint32_t o = a & 0x01FFFFFF;
  if (o >= kMem1Bytes) return 0;
  if (__builtin_expect(g_read_log != nullptr, 0)) log_r(g_m1 + o, 1);
  return g_m1[o];
}

inline int16_t clamp16(int64_t v) {
  return (int16_t)std::clamp<int64_t>(v, -0x8000, 0x7FFF);
}

inline v8si clamp16v(v8si v) {
  const v8si lo = {-0x8000, -0x8000, -0x8000, -0x8000, -0x8000, -0x8000, -0x8000, -0x8000};
  const v8si hi = {0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF, 0x7FFF};
  v = v < lo ? lo : v;
  return v > hi ? hi : v;
}

inline v8si load8(const int16_t *p) {
  v8hi h;
  std::memcpy(&h, p, sizeof h);
  return __builtin_convertvector(h, v8si);
}

// Per-sample 16-bit volume v0, v0+d, v0+2d, ... (mod 2^16) for lanes 0..7.
inline v8si ramp8(uint16_t v0, uint16_t d) {
  const v8si idx = {0, 1, 2, 3, 4, 5, 6, 7};
  return (idx * (int32_t)d + (int32_t)v0) & 0xFFFF;
}

// ---- voice parameter block (AXPBWii, 0x4cc52064 layout: no `updates`) ----
// Indices are in 16-bit words.
enum PB : int {
  NEXT_HI = 0x00, NEXT_LO = 0x01,
  SRC_TYPE = 0x04, COEF_SELECT = 0x05,
  MIX_CTRL_HI = 0x06, MIX_CTRL_LO = 0x07,
  RUNNING = 0x08, IS_STREAM = 0x09,
  // mixer: {volume, delta} pairs
  MIX_MAIN_L = 0x0A, MIX_MAIN_R = 0x0C, MIX_AUXA_L = 0x0E, MIX_AUXA_R = 0x10,
  MIX_AUXB_L = 0x12, MIX_AUXB_R = 0x14, MIX_AUXC_L = 0x16, MIX_AUXC_R = 0x18,
  MIX_MAIN_S = 0x1A, MIX_AUXA_S = 0x1C, MIX_AUXB_S = 0x1E, MIX_AUXC_S = 0x20,
  // de-pop: last sample sent to each bus
  DPOP_MAIN_L = 0x29, DPOP_AUXA_L = 0x2A, DPOP_AUXB_L = 0x2B, DPOP_AUXC_L = 0x2C,
  DPOP_MAIN_R = 0x2D, DPOP_AUXA_R = 0x2E, DPOP_AUXB_R = 0x2F, DPOP_AUXC_R = 0x30,
  DPOP_MAIN_S = 0x31, DPOP_AUXA_S = 0x32, DPOP_AUXB_S = 0x33, DPOP_AUXC_S = 0x34,
  VOL_ENV = 0x35, VOL_ENV_DELTA = 0x36,
  LOOPING = 0x37, SAMPLE_FORMAT = 0x38,
  LOOP_HI = 0x39, LOOP_LO = 0x3A, END_HI = 0x3B, END_LO = 0x3C,
  CUR_HI = 0x3D, CUR_LO = 0x3E,
  ADPCM_COEFS = 0x3F,  // 16 words
  ADPCM_GAIN = 0x4F, ADPCM_PRED_SCALE = 0x50, ADPCM_YN1 = 0x51, ADPCM_YN2 = 0x52,
  SRC_RATIO_HI = 0x53, SRC_RATIO_LO = 0x54, SRC_FRAC = 0x55,
  SRC_LAST = 0x56,  // 4 words
  LOOP_PRED_SCALE = 0x5A, LOOP_YN1 = 0x5B, LOOP_YN2 = 0x5C,
  LPF_ON = 0x5D, LPF_YN1 = 0x5E, LPF_A0 = 0x5F, LPF_B0 = 0x60,
  BQ_ON = 0x61, BQ_XN1 = 0x62, BQ_XN2 = 0x63, BQ_YN1 = 0x64, BQ_YN2 = 0x65,
  BQ_B0 = 0x66, BQ_B1 = 0x67, BQ_B2 = 0x68, BQ_A1 = 0x69, BQ_A2 = 0x6A,
  PB_WORDS = 0x96,
};

enum : uint16_t { SRC_POLYPHASE = 0, SRC_LINEAR = 1, SRC_NEAREST = 2 };

uint32_t hilo(const uint16_t *pb, int hi) { return ((uint32_t)pb[hi] << 16) | pb[hi + 1]; }

// ---- DSP accelerator: sample fetch + ADPCM/PCM decode with loop handling ----

struct Accelerator {
  uint32_t start = 0, end = 0, cur = 0;
  uint16_t format = 0;
  int16_t yn1 = 0, yn2 = 0, gain = 0;
  uint16_t pred_scale = 0;
  bool stopped = false;
  uint16_t *pb = nullptr;

  void set_yn2(int16_t v) {
    yn2 = v;
    stopped = false;
  }

  uint16_t current_sample() const {
    switch (format & 3) {
    case 0: {  // 4-bit
      uint16_t v = aram8(cur >> 1);
      return (cur & 1) ? (v & 0xF) : (v >> 4);
    }
    case 1:  // 8-bit
      return aram8(cur);
    case 2:  // 16-bit
      return (uint16_t)((aram8(cur * 2) << 8) | aram8(cur * 2 + 1));
    default:
      return 0;
    }
  }

  // Reached the end address: loop or stop the voice.
  void on_end() {
    if (pb[LOOPING]) {
      pred_scale = pb[LOOP_PRED_SCALE] & 0x7F;
      if (pb[IS_STREAM] != 1) {
        yn1 = (int16_t)pb[LOOP_YN1];
        set_yn2((int16_t)pb[LOOP_YN2]);
      } else {
        // Streams keep decoding history across the loop seam.
        set_yn2(yn2);
      }
    } else {
      pb[RUNNING] = 0;
    }
  }

  inline int16_t read_sample(const int16_t *coefs) {
    if (stopped)
      return 0;
    const unsigned decode = (format >> 2) & 3;
    const unsigned gain_scale = (format >> 4) & 3;
    int16_t raw = (int16_t)current_sample();
    const int ci = (pred_scale >> 4) & 7;
    const int32_t c1 = coefs[ci * 2], c2 = coefs[ci * 2 + 1];
    int16_t val = 0;
    uint32_t step = 0;
    if (decode == 0) {  // DSP-ADPCM
      raw &= 0xF;
      const int32_t scale = 1 << (pred_scale & 0xF);
      if (raw >= 8)
        raw -= 16;
      const int32_t v = scale * raw + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
      val = clamp16(v);
      step = 2;
      yn2 = yn1;
      yn1 = val;
      cur += 1;
      if ((end & 0xF) == 0 && cur == end) {
        cur = start + 1;
      } else if ((end & 0xF) == 1 && cur == end - 1) {
        cur = start;
      } else if ((cur & 15) == 0) {  // next 8-byte frame header
        pred_scale = aram8((cur & ~15u) >> 1);
        cur += 2;
        step += 2;
      }
    } else {  // PCM (the MMIO input variants never occur in AX voices)
      const int shift = gain_scale == 0 ? 11 : gain_scale == 2 ? 16 : 0;
      const int32_t v = ((int32_t)gain * raw >> shift) + ((c1 * yn1) >> shift) +
                        ((c2 * yn2) >> shift);
      val = (int16_t)v;
      yn2 = yn1;
      yn1 = val;
      step = 2;
      if (decode != 1)
        cur += 1;
    }
    if (cur == end + step - 1) {
      cur = start;
      stopped = true;
      on_end();
    }
    cur &= 0xBFFFFFFF;
    return val;
  }
};

// Sample-rate conversion. No DSP-ROM polyphase table ships with the runtime,
// so polyphase falls back to linear, as Dolphin does without dsp_coef.bin.
template <typename Input>
inline uint32_t resample(Input &&input, int16_t *out, int count, uint16_t *last,
                         uint32_t pos, uint32_t ratio, uint16_t type) {
  if (type == SRC_NEAREST) {
    for (int i = 0; i < count; ++i)
      out[i] = input();
    for (int i = 0; i < 4; ++i)
      last[i] = (uint16_t)out[count - 4 + i];
    return pos;
  }
  int16_t t[4];
  uint32_t idx = 0;
  for (int i = 0; i < 4; ++i)
    t[idx++ & 3] = (int16_t)last[i];
  for (int i = 0; i < count; ++i) {
    pos += ratio;
    while (pos >= 0x10000) {
      t[idx++ & 3] = input();
      pos -= 0x10000;
    }
    const uint16_t frac = pos & 0xFFFF;
    if (frac) {
      const int32_t s0 = t[idx++ & 3];
      const int32_t s1 = t[idx++ & 3];
      out[i] = (int16_t)((s0 * (uint16_t)-frac + s1 * frac) >> 16);
      idx += 2;
    } else {
      out[i] = t[idx++ & 3];
      idx += 3;
    }
  }
  last[3] = (uint16_t)t[--idx & 3];
  last[2] = (uint16_t)t[--idx & 3];
  last[1] = (uint16_t)t[--idx & 3];
  last[0] = (uint16_t)t[--idx & 3];
  return pos;
}

// ---- mixing buses ----

struct Buses {
  alignas(32) int32_t main_l[kFrame], main_r[kFrame], main_s[kFrame];
  int32_t auxa_l[kFrame], auxa_r[kFrame], auxa_s[kFrame];
  int32_t auxb_l[kFrame], auxb_r[kFrame], auxb_s[kFrame];
  int32_t auxc_l[kFrame], auxc_r[kFrame], auxc_s[kFrame];
  int32_t wm[8][kWmFrame];  // main0, aux0, main1, aux1, ... (remote speakers)
};

Buses g_bus;
uint16_t g_last_main_volume = 0x8000;
uint16_t g_last_aux_volume[3] = {0x8000, 0x8000, 0x8000};
uint16_t g_compressor_pos = 0;

void volume_ramp(uint16_t *out, uint16_t from, uint16_t to) {
  float v = from;
  const float step = ((int)to - (int)from) / (float)kFrame;
  for (int i = 0; i < kFrame; ++i) {
    v += step;
    out[i] = (uint16_t)v;
  }
}

// out[i] += clamp16((in[i] * volume_i) >> 15), volume_i = v0 + i*delta.
void mix_add(int32_t *out, const int16_t *in, uint16_t *pb, int vol_idx, int dpop_idx,
             bool ramp) {
  const uint16_t v0 = pb[vol_idx];
  const uint16_t delta = ramp ? pb[vol_idx + 1] : 0;
  v8si vol = ramp8(v0, delta);
  const int32_t step8 = (int32_t)(uint16_t)(delta * 8);
  for (int i = 0; i < kFrame; i += 8) {
    const v8si r = clamp16v((load8(in + i) * vol) >> 15);
    v8si o;
    std::memcpy(&o, out + i, sizeof o);
    o += r;
    std::memcpy(out + i, &o, sizeof o);
    vol = (vol + step8) & 0xFFFF;
  }
  const uint16_t vlast = (uint16_t)(v0 + (kFrame - 1) * delta);
  pb[vol_idx] = (uint16_t)(v0 + kFrame * delta);
  pb[dpop_idx] = (uint16_t)clamp16(((int32_t)in[kFrame - 1] * vlast) >> 15);
}

// Volume envelope (unsigned on Wii): s[i] = clamp16((s[i] * env_i) >> 15).
void envelope(int16_t *s, uint16_t *pb) {
  const uint16_t e0 = pb[VOL_ENV], d = pb[VOL_ENV_DELTA];
  v8si env = ramp8(e0, d);
  const int32_t step8 = (int32_t)(uint16_t)(d * 8);
  for (int i = 0; i < kFrame; i += 8) {
    const v8si r = clamp16v((load8(s + i) * env) >> 15);
    const v8hi h = __builtin_convertvector(r, v8hi);
    std::memcpy(s + i, &h, sizeof h);
    env = (env + step8) & 0xFFFF;
  }
  pb[VOL_ENV] = (uint16_t)(e0 + kFrame * d);
}

void low_pass(int16_t *s, uint16_t *pb) {
  int16_t yn1 = (int16_t)pb[LPF_YN1];
  const int32_t a0 = pb[LPF_A0], b0 = (int16_t)pb[LPF_B0];
  for (int i = 0; i < kFrame; ++i)
    yn1 = s[i] = clamp16((a0 * s[i] + b0 * yn1) >> 15);
  pb[LPF_YN1] = (uint16_t)yn1;
}

void biquad(int16_t *s, uint16_t *pb) {
  int16_t xn1 = (int16_t)pb[BQ_XN1], xn2 = (int16_t)pb[BQ_XN2];
  int16_t yn1 = (int16_t)pb[BQ_YN1], yn2 = (int16_t)pb[BQ_YN2];
  const int32_t b0 = (int16_t)pb[BQ_B0], b1 = (int16_t)pb[BQ_B1], b2 = (int16_t)pb[BQ_B2];
  const int32_t a1 = (int16_t)pb[BQ_A1], a2 = (int16_t)pb[BQ_A2];
  for (int i = 0; i < kFrame; ++i) {
    const int16_t xn0 = s[i];
    int64_t t = (int64_t)b0 * xn0 + (int64_t)b1 * xn1 + (int64_t)b2 * xn2 +
                (int64_t)a1 * yn1 + (int64_t)a2 * yn2;
    t <<= 2;
    t += (t & 0x10000) ? 0x8000 : 0x7FFF;
    const int16_t yn0 = clamp16(t >> 16);
    xn2 = xn1;
    yn2 = yn1;
    xn1 = xn0;
    yn1 = yn0;
    s[i] = yn0;
  }
  pb[BQ_XN1] = (uint16_t)xn1;
  pb[BQ_XN2] = (uint16_t)xn2;
  pb[BQ_YN1] = (uint16_t)yn1;
  pb[BQ_YN2] = (uint16_t)yn2;
}

void process_voice(uint16_t *pb) {
  Accelerator acc;
  acc.pb = pb;
  acc.start = hilo(pb, LOOP_HI) & 0x3FFFFFFF;
  acc.end = hilo(pb, END_HI) & 0x3FFFFFFF;
  acc.cur = hilo(pb, CUR_HI) & 0xBFFFFFFF;
  acc.format = pb[SAMPLE_FORMAT];
  acc.yn1 = (int16_t)pb[ADPCM_YN1];
  acc.set_yn2((int16_t)pb[ADPCM_YN2]);
  acc.gain = (int16_t)pb[ADPCM_GAIN];
  acc.pred_scale = pb[ADPCM_PRED_SCALE] & 0x7F;

  int16_t coefs[16];
  for (int i = 0; i < 16; ++i)
    coefs[i] = (int16_t)pb[ADPCM_COEFS + i];

  alignas(16) int16_t s[kFrame];
  const uint32_t pos = resample([&] { return acc.read_sample(coefs); }, s, kFrame,
                                &pb[SRC_LAST], pb[SRC_FRAC], hilo(pb, SRC_RATIO_HI),
                                pb[SRC_TYPE]);
  pb[SRC_FRAC] = (uint16_t)pos;
  pb[CUR_HI] = (uint16_t)(acc.cur >> 16);
  pb[CUR_LO] = (uint16_t)acc.cur;
  pb[ADPCM_YN1] = (uint16_t)acc.yn1;
  pb[ADPCM_YN2] = (uint16_t)acc.yn2;
  pb[ADPCM_PRED_SCALE] = acc.pred_scale;

  envelope(s, pb);

  if (pb[LPF_ON])
    low_pass(s, pb);
  if (pb[BQ_ON])
    biquad(s, pb);

  // Mixer control bits (AXWii): per bus "on" bit and a combined on+ramp bit.
  const uint32_t mc = hilo(pb, MIX_CTRL_HI);
  struct Route {
    int32_t *bus;
    int vol, dpop;
    uint32_t on, ramp;
  };
  const Route routes[] = {
      {g_bus.main_l, MIX_MAIN_L, DPOP_MAIN_L, 0x00000001 | 0x00000004, 0x00000004},
      {g_bus.main_r, MIX_MAIN_R, DPOP_MAIN_R, 0x00000002 | 0x00000004, 0x00000004},
      {g_bus.main_s, MIX_MAIN_S, DPOP_MAIN_S, 0x00000008 | 0x00000010, 0x00000010},
      {g_bus.auxa_l, MIX_AUXA_L, DPOP_AUXA_L, 0x00010000 | 0x00040000, 0x00040000},
      {g_bus.auxa_r, MIX_AUXA_R, DPOP_AUXA_R, 0x00020000 | 0x00040000, 0x00040000},
      {g_bus.auxa_s, MIX_AUXA_S, DPOP_AUXA_S, 0x00080000 | 0x00100000, 0x00100000},
      {g_bus.auxb_l, MIX_AUXB_L, DPOP_AUXB_L, 0x00200000 | 0x00800000, 0x00800000},
      {g_bus.auxb_r, MIX_AUXB_R, DPOP_AUXB_R, 0x00400000 | 0x00800000, 0x00800000},
      {g_bus.auxb_s, MIX_AUXB_S, DPOP_AUXB_S, 0x01000000 | 0x02000000, 0x02000000},
      {g_bus.auxc_l, MIX_AUXC_L, DPOP_AUXC_L, 0x04000000 | 0x10000000, 0x10000000},
      {g_bus.auxc_r, MIX_AUXC_R, DPOP_AUXC_R, 0x08000000 | 0x10000000, 0x10000000},
      {g_bus.auxc_s, MIX_AUXC_S, DPOP_AUXC_S, 0x20000000 | 0x40000000, 0x40000000},
  };
  for (const Route &r : routes)
    if (mc & r.on)
      mix_add(r.bus, s, pb, r.vol, r.dpop, (mc & r.ramp) != 0);
  // Wii Remote speaker output (pb remote fields) is not emulated.
}

// ---- r43a (frente A): mezclador rapido, NWII_A_MIX=1 ----------------------------------------
// Mismo algoritmo bit a bit que process_voice (lo comprueba NWII_AX_VERIFY=1 contra ref::). En la Switch
// (-O2, cortex-a57) Accelerator::read_sample y aram8 no se integraban: dos llamadas con marco por muestra,
// g_m1/g_m2/g_read_log recargados en cada una y el registro de lecturas (vector) dentro de aram8 (664
// bytes). Aqui todo va integrado, con MEM1/MEM2 en locales y el formato de muestra fijado por plantilla
// (ADPCM, PCM16, PCM8; el resto, generico). Solo sin registro de lecturas (g_read_log: NWII_AX_HAZARD).
// Voces paradas: solo se leen NEXT y RUNNING (antes, las 150 palabras; no se escribia nada igual).
#define A43_INL inline __attribute__((always_inline))
struct FastAcc {
  const uint8_t *m1, *m2;
  uint16_t *pb;
  uint32_t start, end, cur;
  uint16_t format, pred_scale;
  int16_t yn1, yn2, gain;
  bool stopped;

  A43_INL uint8_t a8(uint32_t a) const {
    if (a & 0x10000000) return m2[a & 0x03FFFFFF];
    const uint32_t o = a & 0x01FFFFFF;
    return o < kMem1Bytes ? m1[o] : 0;
  }
  template <int F3> A43_INL uint16_t cur_sample() const {
    const int f3 = F3 >= 0 ? F3 : (int)(format & 3);
    if (f3 == 0) {
      const uint16_t v = a8(cur >> 1);
      return (cur & 1) ? (v & 0xF) : (v >> 4);
    }
    if (f3 == 1) return a8(cur);
    if (f3 == 2) return (uint16_t)((a8(cur * 2) << 8) | a8(cur * 2 + 1));
    return 0;
  }
  A43_INL void on_end() {  // = Accelerator::on_end
    if (pb[LOOPING]) {
      pred_scale = pb[LOOP_PRED_SCALE] & 0x7F;
      if (pb[IS_STREAM] != 1) {
        yn1 = (int16_t)pb[LOOP_YN1];
        yn2 = (int16_t)pb[LOOP_YN2];
      }
      stopped = false;  // set_yn2
    } else {
      pb[RUNNING] = 0;
    }
  }
  // = Accelerator::read_sample
  template <int F3, int DEC> A43_INL int16_t read(const int16_t *coefs) {
    if (stopped) return 0;
    const unsigned decode = DEC >= 0 ? (unsigned)DEC : (format >> 2) & 3;
    int16_t raw = (int16_t)cur_sample<F3>();
    const int ci = (pred_scale >> 4) & 7;
    const int32_t c1 = coefs[ci * 2], c2 = coefs[ci * 2 + 1];
    int16_t val;
    uint32_t step;
    if (decode == 0) {
      raw &= 0xF;
      const int32_t scale = 1 << (pred_scale & 0xF);
      if (raw >= 8) raw -= 16;
      const int32_t v = scale * raw + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
      val = clamp16(v);
      step = 2;
      yn2 = yn1;
      yn1 = val;
      cur += 1;
      if ((end & 0xF) == 0 && cur == end) {
        cur = start + 1;
      } else if ((end & 0xF) == 1 && cur == end - 1) {
        cur = start;
      } else if ((cur & 15) == 0) {
        pred_scale = a8((cur & ~15u) >> 1);
        cur += 2;
        step += 2;
      }
    } else {
      const unsigned gain_scale = (format >> 4) & 3;
      const int shift = gain_scale == 0 ? 11 : gain_scale == 2 ? 16 : 0;
      const int32_t v = ((int32_t)gain * raw >> shift) + ((c1 * yn1) >> shift) + ((c2 * yn2) >> shift);
      val = (int16_t)v;
      yn2 = yn1;
      yn1 = val;
      step = 2;
      if (decode != 1) cur += 1;
    }
    if (cur == end + step - 1) {
      cur = start;
      stopped = true;
      on_end();
    }
    cur &= 0xBFFFFFFF;
    return val;
  }
  // NWII_A_MIX=2: `cnt` muestras DSP-ADPCM, exactamente como cnt llamadas a read<0, 0>(coefs). Dentro de una trama
  // de 8 bytes (cur & 15 en [2, 15)) y lejos del final, solo hay que decodificar el nibble y avanzar: ese tramo va
  // en un bucle corto (coeficientes y escala fijos, puntero de la trama fijo); el nibble que cierra la trama, el
  // final de la muestra y lo raro van por read<0, 0>.
  A43_INL void adpcm_batch(int16_t *out, uint32_t cnt, const int16_t *coefs) {
    uint32_t k = 0;
    while (k < cnt) {
      if (stopped) {  // read devuelve 0 sin tocar nada mientras la voz esta parada
        for (; k < cnt; ++k) out[k] = 0;
        return;
      }
      const uint32_t in_fr = cur & 15;
      uint32_t m = 0;
      const uint8_t *hb = nullptr;
      const uint32_t ba = cur >> 1;
      if (in_fr >= 2 && in_fr < 15) {
        m = 15 - in_fr;
        if (end >= cur && end - cur < m) m = end - cur;
        if (m > cnt - k) m = cnt - k;
        if (ba & 0x10000000) hb = m2 + (ba & 0x03FFFFFF);
        else if ((ba & 0x01FFFFFF) < kMem1Bytes) hb = m1 + (ba & 0x01FFFFFF);
        else m = 0;
      }
      if (m) {
        const int ci = (pred_scale >> 4) & 7;
        const int32_t c1 = coefs[ci * 2], c2 = coefs[ci * 2 + 1];
        const int32_t scale = 1 << (pred_scale & 0xF);
        int32_t y1 = yn1, y2 = yn2;
        uint32_t c = cur;
        for (uint32_t j = 0; j < m; ++j, ++c) {
          const uint8_t b = hb[(c >> 1) - ba];
          int32_t raw = (c & 1) ? (b & 0xF) : (b >> 4);
          if (raw >= 8) raw -= 16;
          const int16_t val = clamp16(scale * raw + ((0x400 + c1 * y1 + c2 * y2) >> 11));
          y2 = y1;
          y1 = val;
          out[k + j] = val;
        }
        yn1 = (int16_t)y1;
        yn2 = (int16_t)y2;
        cur = c;
        k += m;
        continue;
      }
      out[k++] = read<0, 0>(coefs);
    }
  }
};

int a43_mix_mode() {
  static const int m = [] {
    const char *e = std::getenv("NWII_A_MIX");
    return (e && *e && *e != '0') ? (*e == '2' ? 2 : 1) : 0;
  }();
  return m;
}

template <int F3, int DEC>
void process_voice_fast(uint16_t *pb) {
  FastAcc acc;
  acc.m1 = g_m1;
  acc.m2 = g_m2;
  acc.pb = pb;
  acc.start = hilo(pb, LOOP_HI) & 0x3FFFFFFF;
  acc.end = hilo(pb, END_HI) & 0x3FFFFFFF;
  acc.cur = hilo(pb, CUR_HI) & 0xBFFFFFFF;
  acc.format = pb[SAMPLE_FORMAT];
  acc.yn1 = (int16_t)pb[ADPCM_YN1];
  acc.yn2 = (int16_t)pb[ADPCM_YN2];
  acc.stopped = false;
  acc.gain = (int16_t)pb[ADPCM_GAIN];
  acc.pred_scale = pb[ADPCM_PRED_SCALE] & 0x7F;
  int16_t coefs[16];
  for (int i = 0; i < 16; ++i) coefs[i] = (int16_t)pb[ADPCM_COEFS + i];
  alignas(16) int16_t s[kFrame];
  uint32_t pos;
  // NWII_A_MIX=2 (ADPCM): primero las muestras de entrada que pedira el remuestreo (cuantas: solo depende de la
  // fraccion, la razon y el tipo), en lote; luego el remuestreo de siempre leyendolas del bufer.
  constexpr uint32_t kMaxBatch = 2048;
  uint32_t total = kMaxBatch + 1;
  if constexpr (F3 == 0 && DEC == 0) {
    if (a43_mix_mode() == 2) {
      const uint32_t ratio = hilo(pb, SRC_RATIO_HI);
      if (pb[SRC_TYPE] == SRC_NEAREST) {
        total = kFrame;
      } else if (ratio < 0x01000000u) {
        uint32_t p = pb[SRC_FRAC];
        total = 0;
        for (int i = 0; i < kFrame; ++i) {
          p += ratio;
          total += p >> 16;
          p &= 0xFFFF;
        }
      }
    }
  }
  if (total <= kMaxBatch) {
    alignas(16) int16_t in[kMaxBatch];
    acc.adpcm_batch(in, total, coefs);
    uint32_t k = 0;
    pos = resample([&]() __attribute__((always_inline)) { return in[k++]; }, s, kFrame, &pb[SRC_LAST], pb[SRC_FRAC],
                   hilo(pb, SRC_RATIO_HI), pb[SRC_TYPE]);
  } else {
    pos = resample([&]() __attribute__((always_inline)) { return acc.read<F3, DEC>(coefs); }, s, kFrame,
                   &pb[SRC_LAST], pb[SRC_FRAC], hilo(pb, SRC_RATIO_HI), pb[SRC_TYPE]);
  }
  pb[SRC_FRAC] = (uint16_t)pos;
  pb[CUR_HI] = (uint16_t)(acc.cur >> 16);
  pb[CUR_LO] = (uint16_t)acc.cur;
  pb[ADPCM_YN1] = (uint16_t)acc.yn1;
  pb[ADPCM_YN2] = (uint16_t)acc.yn2;
  pb[ADPCM_PRED_SCALE] = acc.pred_scale;
  envelope(s, pb);
  if (pb[LPF_ON]) low_pass(s, pb);
  if (pb[BQ_ON]) biquad(s, pb);
  const uint32_t mc = hilo(pb, MIX_CTRL_HI);
  struct Route {
    int32_t *bus;
    int vol, dpop;
    uint32_t on, ramp;
  };
  static const Route routes[] = {
      {g_bus.main_l, MIX_MAIN_L, DPOP_MAIN_L, 0x00000001 | 0x00000004, 0x00000004},
      {g_bus.main_r, MIX_MAIN_R, DPOP_MAIN_R, 0x00000002 | 0x00000004, 0x00000004},
      {g_bus.main_s, MIX_MAIN_S, DPOP_MAIN_S, 0x00000008 | 0x00000010, 0x00000010},
      {g_bus.auxa_l, MIX_AUXA_L, DPOP_AUXA_L, 0x00010000 | 0x00040000, 0x00040000},
      {g_bus.auxa_r, MIX_AUXA_R, DPOP_AUXA_R, 0x00020000 | 0x00040000, 0x00040000},
      {g_bus.auxa_s, MIX_AUXA_S, DPOP_AUXA_S, 0x00080000 | 0x00100000, 0x00100000},
      {g_bus.auxb_l, MIX_AUXB_L, DPOP_AUXB_L, 0x00200000 | 0x00800000, 0x00800000},
      {g_bus.auxb_r, MIX_AUXB_R, DPOP_AUXB_R, 0x00400000 | 0x00800000, 0x00800000},
      {g_bus.auxb_s, MIX_AUXB_S, DPOP_AUXB_S, 0x01000000 | 0x02000000, 0x02000000},
      {g_bus.auxc_l, MIX_AUXC_L, DPOP_AUXC_L, 0x04000000 | 0x10000000, 0x10000000},
      {g_bus.auxc_r, MIX_AUXC_R, DPOP_AUXC_R, 0x08000000 | 0x10000000, 0x10000000},
      {g_bus.auxc_s, MIX_AUXC_S, DPOP_AUXC_S, 0x20000000 | 0x40000000, 0x40000000},
  };
  for (const Route &r : routes)
    if (mc & r.on) mix_add(r.bus, s, pb, r.vol, r.dpop, (mc & r.ramp) != 0);
}

void process_voice_a43(uint16_t *pb) {
  switch (pb[SAMPLE_FORMAT] & 0xF) {  // (format & 3) | decode << 2
    case 0x0: process_voice_fast<0, 0>(pb); break;   // DSP-ADPCM
    case 0xA: process_voice_fast<2, 2>(pb); break;   // PCM16
    case 0x9: process_voice_fast<1, 2>(pb); break;   // PCM8
    default: process_voice_fast<-1, -1>(pb); break;
  }
}

bool a43_mix_on() { return a43_mix_mode() != 0; }

void process_pb_list(uint32_t pb_addr) {
  if (a43_mix_on() && g_read_log == nullptr) {
    uint16_t pb[PB_WORDS], orig[PB_WORDS];
    for (int n = 0; pb_addr && n < kMaxVoices; ++n) {
      uint8_t *p = span(pb_addr, PB_WORDS * 2);
      if (!p) {  // no contiguo: camino de siempre para este bloque
        for (int i = 0; i < PB_WORDS; ++i) pb[i] = rd16(pb_addr + i * 2);
        if (pb[RUNNING] == 1) {
          std::memcpy(orig, pb, sizeof pb);
          process_voice_a43(pb);
          for (int i = 0; i < PB_WORDS; ++i)
            if (pb[i] != orig[i]) wr16(pb_addr + i * 2, pb[i]);
        }
        pb_addr = hilo(pb, NEXT_HI);
        continue;
      }
      if (be16(p + RUNNING * 2) != 1) {
        pb_addr = ((uint32_t)be16(p) << 16) | be16(p + 2);
        continue;
      }
      for (int i = 0; i < PB_WORDS; ++i) pb[i] = be16(p + i * 2);
      std::memcpy(orig, pb, sizeof pb);
      process_voice_a43(pb);
      for (int i = 0; i < PB_WORDS; ++i) {
        if (pb[i] == orig[i]) continue;
        put16(p + i * 2, pb[i]);
        log_w(p + i * 2, 2);
      }
      pb_addr = hilo(pb, NEXT_HI);
    }
    return;
  }
  uint16_t pb[PB_WORDS], orig[PB_WORDS];
  for (int n = 0; pb_addr && n < kMaxVoices; ++n) {
    uint8_t *p = span(pb_addr, PB_WORDS * 2);
    if (p) {
      log_r(p, PB_WORDS * 2);
      for (int i = 0; i < PB_WORDS; ++i)
        pb[i] = be16(p + i * 2);
    } else {
      for (int i = 0; i < PB_WORDS; ++i)
        pb[i] = rd16(pb_addr + i * 2);
    }
    if (pb[RUNNING] == 1) {
      std::memcpy(orig, pb, sizeof pb);
      process_voice(pb);
      // Write back what changed (the rest already holds these values).
      for (int i = 0; i < PB_WORDS; ++i) {
        if (pb[i] == orig[i]) continue;
        if (p) {
          put16(p + i * 2, pb[i]);
          log_w(p + i * 2, 2);
        } else {
          wr16(pb_addr + i * 2, pb[i]);
        }
      }
    }
    pb_addr = hilo(pb, NEXT_HI);
  }
}

void setup(uint32_t init_addr) {
  struct Desc {
    int32_t *buf;
    int count;
  };
  const Desc bufs[20] = {
      {g_bus.main_l, kFrame}, {g_bus.main_r, kFrame}, {g_bus.main_s, kFrame},
      {g_bus.auxa_l, kFrame}, {g_bus.auxa_r, kFrame}, {g_bus.auxa_s, kFrame},
      {g_bus.auxb_l, kFrame}, {g_bus.auxb_r, kFrame}, {g_bus.auxb_s, kFrame},
      {g_bus.auxc_l, kFrame}, {g_bus.auxc_r, kFrame}, {g_bus.auxc_s, kFrame},
      {g_bus.wm[0], kWmFrame}, {g_bus.wm[1], kWmFrame}, {g_bus.wm[2], kWmFrame},
      {g_bus.wm[3], kWmFrame}, {g_bus.wm[4], kWmFrame}, {g_bus.wm[5], kWmFrame},
      {g_bus.wm[6], kWmFrame}, {g_bus.wm[7], kWmFrame},
  };
  uint16_t w[60];
  load16(init_addr, w, 60);
  for (int i = 0; i < 20; ++i) {
    const int32_t value = (int32_t)(((uint32_t)w[i * 3] << 16) | w[i * 3 + 1]);
    const int16_t delta = (int16_t)w[i * 3 + 2];
    for (int j = 0; j < bufs[i].count; ++j)
      bufs[i].buf[j] = value ? value + j * delta : 0;
  }
}

void add_to_lr(uint32_t addr, bool neg) {
  int32_t v[kFrame];
  load32(addr, v, kFrame);
  for (int i = 0; i < kFrame; ++i) {
    const int32_t x = neg ? -v[i] : v[i];
    g_bus.main_l[i] += x;
    g_bus.main_r[i] += x;
  }
}

void add_sub_to_lr(uint32_t addr) {
  int32_t v[2 * kFrame];
  load32(addr, v, 2 * kFrame);
  for (int i = 0; i < kFrame; ++i)
    g_bus.main_l[i] += v[i];
  for (int i = 0; i < kFrame; ++i)
    g_bus.main_r[i] -= v[kFrame + i];
}

void write_bus(uint32_t addr, const int32_t *bus) { store32(addr, bus, kFrame); }

// Send an aux bus to the CPU (effects run there) and mix back what the CPU
// returned for it last frame.
void mix_aux(int id, uint32_t write_addr, uint32_t read_addr, uint16_t volume) {
  uint16_t ramp[kFrame];
  volume_ramp(ramp, g_last_aux_volume[id], volume);
  g_last_aux_volume[id] = volume;
  int32_t *aux[3][3] = {{g_bus.auxa_l, g_bus.auxa_r, g_bus.auxa_s},
                        {g_bus.auxb_l, g_bus.auxb_r, g_bus.auxb_s},
                        {g_bus.auxc_l, g_bus.auxc_r, g_bus.auxc_s}};
  if (write_addr)
    for (int c = 0; c < 3; ++c)
      write_bus(write_addr + c * kFrame * 4, aux[id][c]);
  int32_t in[3 * kFrame];
  load32(read_addr, in, 3 * kFrame);
  int32_t *main[3] = {g_bus.main_l, g_bus.main_r, g_bus.main_s};
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < kFrame; ++i)
      main[c][i] += (int32_t)(((int64_t)in[c * kFrame + i] * ramp[i]) >> 15);
}

void upload_aux_mix_lrsc(int id, const uint32_t *addr, uint16_t volume) {
  int32_t *l = id ? g_bus.auxb_l : g_bus.auxa_l;
  int32_t *r = id ? g_bus.auxb_r : g_bus.auxa_r;
  int32_t *s = id ? g_bus.auxb_s : g_bus.auxa_s;
  int32_t *c = id ? g_bus.auxc_s : g_bus.auxc_r;
  write_bus(addr[0], l);
  write_bus(addr[0] + kFrame * 4, r);
  write_bus(addr[0] + 2 * kFrame * 4, s);
  write_bus(addr[1], c);
  uint16_t ramp[kFrame];
  volume_ramp(ramp, g_last_aux_volume[id], volume);
  g_last_aux_volume[id] = volume;
  int32_t *dest[4] = {g_bus.main_l, g_bus.main_r, g_bus.main_s, g_bus.auxc_l};
  int32_t in[kFrame];
  for (int m = 0; m < 4; ++m) {
    load32(addr[2 + m], in, kFrame);
    for (int i = 0; i < kFrame; ++i)
      dest[m][i] += (int32_t)(((int64_t)in[i] * ramp[i]) >> 15);
  }
}

void compressor(uint16_t threshold, uint16_t release_frames, uint32_t table) {
  bool triggered = false;
  for (int i = 0; i < kFrame && !triggered; ++i)
    triggered = std::abs(g_bus.main_l[i]) > threshold || std::abs(g_bus.main_r[i]) > threshold;
  const uint32_t frame_bytes = kFrame * 2;
  uint32_t offset;
  if (triggered) {
    offset = g_compressor_pos * frame_bytes;
    g_compressor_pos = release_frames;
  } else if (g_compressor_pos) {
    --g_compressor_pos;
    constexpr uint32_t kAttackEntries = 11;
    offset = (kAttackEntries + g_compressor_pos) * frame_bytes;
  } else {
    return;
  }
  uint16_t coef[kFrame];
  load16(table + offset, coef, kFrame);
  for (int i = 0; i < kFrame; ++i) {
    g_bus.main_l[i] = (int32_t)(((int64_t)g_bus.main_l[i] * coef[i]) >> 15);
    g_bus.main_r[i] = (int32_t)(((int64_t)g_bus.main_r[i] * coef[i]) >> 15);
  }
}

void output(uint32_t lr_addr, uint32_t surround_addr, uint16_t volume, bool dpl2) {
  uint16_t ramp[kFrame];
  volume_ramp(ramp, g_last_main_volume, volume);
  g_last_main_volume = volume;
  write_bus(surround_addr, g_bus.main_s);
  if (dpl2)
    write_bus(surround_addr + kFrame * 4, g_bus.auxc_l);
  // Interleaved R, L as 16-bit big-endian: the layout AI DMA plays.
  uint8_t *p = span(lr_addr, kFrame * 4);
  for (int i = 0; i < kFrame; ++i) {
    const int16_t l = clamp16(((int64_t)g_bus.main_l[i] * ramp[i]) >> 15);
    const int16_t r = clamp16(((int64_t)g_bus.main_r[i] * ramp[i]) >> 15);
    if (p) {
      put16(p + i * 4, (uint16_t)r);
      put16(p + i * 4 + 2, (uint16_t)l);
    } else {
      wr16(lr_addr + i * 4, (uint16_t)r);
      wr16(lr_addr + i * 4 + 2, (uint16_t)l);
    }
  }
  if (p) log_w(p, kFrame * 4);
}

void wm_output(const uint32_t *addr) {
  for (int m = 0; m < 4; ++m) {
    uint8_t *p = span(addr[m], kWmFrame * 2);
    for (int j = 0; j < kWmFrame; ++j) {
      const uint16_t v = (uint16_t)clamp16(g_bus.wm[m * 2][j]);
      if (p) put16(p + j * 2, v);
      else wr16(addr[m] + j * 2, v);
    }
    if (p) log_w(p, kWmFrame * 2);
  }
}

}  // namespace

void state_bytes(std::vector<uint8_t> &out) {
  out.clear();
  auto put = [&](const void *p, size_t n) {
    out.insert(out.end(), (const uint8_t *)p, (const uint8_t *)p + n);
  };
  put(&g_bus, sizeof g_bus);
  put(&g_last_main_volume, sizeof g_last_main_volume);
  put(g_last_aux_volume, sizeof g_last_aux_volume);
  put(&g_compressor_pos, sizeof g_compressor_pos);
}

uint32_t load_command_list(uint32_t addr, uint16_t size_words, uint16_t *out) {
  const uint32_t n = std::min<uint32_t>(size_words, kMaxListWords);
  RangeLog *rl = g_read_log;
  g_read_log = nullptr;  // only the words the list uses count (logged below)
  load16(addr, out, (int)n);
  g_read_log = rl;
  return n;
}

bool run_command_list(uint32_t addr, uint16_t size_words) {
  uint16_t list[kMaxListWords];
  const uint32_t n = load_command_list(addr, size_words, list);
  return run_loaded_list(list, n) ;
}

// Words of the last list up to its END (for the hazard checker's read log).
static uint32_t s_list_used = 0;
uint32_t last_list_words_used() { return s_list_used; }

bool run_loaded_list(const uint16_t *list, uint32_t n) {
  if (!g_mmu)
    return false;
  g_m1 = g_mmu->mem1.data();
  g_m2 = g_mmu->mem2.data();
  bool produced = false;
  uint32_t i = 0;
  auto arg = [&]() -> uint16_t { return i < n ? list[i++] : 0; };
  auto arg32 = [&]() -> uint32_t {
    const uint32_t hi = arg();
    return (hi << 16) | arg();
  };
  while (i < n) {
    const uint16_t cmd = arg();
    switch (cmd) {
    case 0x00:  // SETUP
      setup(arg32());
      break;
    case 0x01:  // ADD_TO_LR
    case 0x02:  // SUB_TO_LR
      add_to_lr(arg32(), cmd == 0x02);
      break;
    case 0x03:  // ADD_SUB_TO_LR
      add_sub_to_lr(arg32());
      break;
    case 0x04:  // PROCESS
      process_pb_list(arg32());
      break;
    case 0x05:  // MIX_AUXA
    case 0x06:  // MIX_AUXB
    case 0x07: {  // MIX_AUXC
      const uint16_t volume = arg();
      const uint32_t write_addr = arg32();
      const uint32_t read_addr = arg32();
      mix_aux(cmd - 0x05, write_addr, read_addr, volume);
      break;
    }
    case 0x08:  // UPL_AUXA_MIX_LRSC
    case 0x09: {  // UPL_AUXB_MIX_LRSC
      const uint16_t volume = arg();
      uint32_t a[6];
      for (uint32_t &x : a)
        x = arg32();
      upload_aux_mix_lrsc(cmd == 0x09, a, volume);
      break;
    }
    case 0x0A: {  // COMPRESSOR
      const uint16_t threshold = arg();
      const uint16_t frames = arg();
      compressor(threshold, frames, arg32());
      break;
    }
    case 0x0B:  // OUTPUT
    case 0x0C: {  // OUTPUT_DPL2
      const uint16_t volume = arg();
      const uint32_t surround = arg32();
      const uint32_t lr = arg32();
      output(lr, surround, volume, cmd == 0x0C);
      produced = true;
      break;
    }
    case 0x0D: {  // WM_OUTPUT
      uint32_t a[4];
      for (uint32_t &x : a)
        x = arg32();
      wm_output(a);
      break;
    }
    case 0x0E:  // END
      s_list_used = i;
      return produced;
    default: {
      static int warned = 0;
      if (warned++ < 8)
        std::cout << "[AX] unknown command 0x" << std::hex << cmd << " at word " << std::dec
                  << (i - 1) << "\n";
      s_list_used = i;
      return produced;
    }
    }
  }
  s_list_used = i;
  return produced;
}

}  // namespace nwii::runtime::ax
