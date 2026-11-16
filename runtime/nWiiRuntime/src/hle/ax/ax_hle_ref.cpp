// R8: reference copy of the original scalar AX HLE (r7b ax_hle.cpp), kept
// only for NWII_AX_VERIFY=1. It runs before the optimized mixer on the same
// command list and never writes guest memory: its writes go to an overlay that
// its own reads see. With no overlay (NWII_AX_MIXER=ref, for timing A/B)
// it is the plain r7b mixer on guest memory. The caller then compares the overlay with what the
// optimized mixer wrote, byte for byte, plus the mixer state (buses, ramps).
#include "ax_hle.h"
#include <unordered_map>

#include "runtime/cpu_context.h"
#include "runtime/nwii_env.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

namespace nwii::runtime {
extern MMU *g_mmu;
}

namespace nwii::runtime::ax::ref {
std::unordered_map<const uint8_t *, uint8_t> *g_overlay = nullptr;
namespace {

constexpr int kFrame = 96;     // samples per 3 ms frame at 32 kHz
constexpr int kWmFrame = 18;   // Wii Remote speaker samples per frame
constexpr int kMaxVoices = 512;

// ---- guest memory (big-endian, physical or cached addresses) ----

uint8_t *mem(uint32_t addr) { return g_mmu ? g_mmu->get_ptr(addr) : nullptr; }

uint8_t ov_rd(const uint8_t *p) {
  if (!g_overlay) return *p;  // NWII_AX_MIXER=ref: plain r7b mixer
  auto it = g_overlay->find(p);
  return it != g_overlay->end() ? it->second : *p;
}

uint16_t rd16(uint32_t a) {
  const uint8_t *p = mem(a);
  return p ? (uint16_t)((ov_rd(p) << 8) | ov_rd(p + 1)) : 0;
}

void wr16(uint32_t a, uint16_t v) {
  if (uint8_t *p = mem(a)) {
    if (!g_overlay) {
      p[0] = (uint8_t)(v >> 8);
      p[1] = (uint8_t)v;
      return;
    }
    (*g_overlay)[p] = (uint8_t)(v >> 8);
    (*g_overlay)[p + 1] = (uint8_t)v;
  }
}

int32_t rd32(uint32_t a) { return (int32_t)(((uint32_t)rd16(a) << 16) | rd16(a + 2)); }

void wr32(uint32_t a, int32_t v) {
  wr16(a, (uint16_t)((uint32_t)v >> 16));
  wr16(a + 2, (uint16_t)v);
}

// The DSP accelerator sees "ARAM" addresses; on Wii those are physical RAM
// addresses, MEM2 when bit 28 is set.
uint8_t aram8(uint32_t a) {
  const uint8_t *p = (a & 0x10000000) ? mem(0x10000000 | (a & 0x03FFFFFF))
                                      : mem(a & 0x01FFFFFF);
  return p ? *p : 0;
}

int16_t clamp16(int64_t v) {
  return (int16_t)std::clamp<int64_t>(v, -0x8000, 0x7FFF);
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

  int16_t read_sample(const int16_t *coefs) {
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
uint32_t resample(Input &&input, int16_t *out, int count, uint16_t *last,
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
  int32_t main_l[kFrame], main_r[kFrame], main_s[kFrame];
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

void mix_add(int32_t *out, const int16_t *in, uint16_t *pb, int vol_idx, int dpop_idx,
             bool ramp) {
  uint16_t volume = pb[vol_idx];
  const uint16_t delta = ramp ? pb[vol_idx + 1] : 0;
  int16_t last = 0;
  for (int i = 0; i < kFrame; ++i) {
    last = clamp16(((int64_t)in[i] * volume) >> 15);
    out[i] += last;
    volume += delta;
  }
  pb[vol_idx] = volume;
  pb[dpop_idx] = (uint16_t)last;
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
  if (pb[RUNNING] != 1)
    return;

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

  int16_t s[kFrame];
  const uint32_t pos = resample([&] { return acc.read_sample(coefs); }, s, kFrame,
                                &pb[SRC_LAST], pb[SRC_FRAC], hilo(pb, SRC_RATIO_HI),
                                pb[SRC_TYPE]);
  pb[SRC_FRAC] = (uint16_t)pos;
  pb[CUR_HI] = (uint16_t)(acc.cur >> 16);
  pb[CUR_LO] = (uint16_t)acc.cur;
  pb[ADPCM_YN1] = (uint16_t)acc.yn1;
  pb[ADPCM_YN2] = (uint16_t)acc.yn2;
  pb[ADPCM_PRED_SCALE] = acc.pred_scale;

  // Volume envelope (unsigned on Wii).
  for (int i = 0; i < kFrame; ++i) {
    s[i] = clamp16(((int32_t)s[i] * (int32_t)pb[VOL_ENV]) >> 15);
    pb[VOL_ENV] = (uint16_t)(pb[VOL_ENV] + pb[VOL_ENV_DELTA]);
  }

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

void process_pb_list(uint32_t pb_addr) {
  uint16_t pb[PB_WORDS];
  for (int n = 0; pb_addr && n < kMaxVoices; ++n) {
    for (int i = 0; i < PB_WORDS; ++i)
      pb[i] = rd16(pb_addr + i * 2);
    process_voice(pb);
    for (int i = 0; i < PB_WORDS; ++i)
      wr16(pb_addr + i * 2, pb[i]);
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
  for (int i = 0; i < 20; ++i) {
    const uint32_t a = init_addr + i * 6;
    const int32_t value = rd32(a);
    const int16_t delta = (int16_t)rd16(a + 4);
    for (int j = 0; j < bufs[i].count; ++j)
      bufs[i].buf[j] = value ? value + j * delta : 0;
  }
}

void add_to_lr(uint32_t addr, bool neg) {
  for (int i = 0; i < kFrame; ++i) {
    int32_t v = rd32(addr + i * 4);
    if (neg)
      v = -v;
    g_bus.main_l[i] += v;
    g_bus.main_r[i] += v;
  }
}

void add_sub_to_lr(uint32_t addr) {
  for (int i = 0; i < kFrame; ++i)
    g_bus.main_l[i] += rd32(addr + i * 4);
  for (int i = 0; i < kFrame; ++i)
    g_bus.main_r[i] -= rd32(addr + kFrame * 4 + i * 4);
}

void write_bus(uint32_t addr, const int32_t *bus) {
  for (int i = 0; i < kFrame; ++i)
    wr32(addr + i * 4, bus[i]);
}

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
  int32_t *main[3] = {g_bus.main_l, g_bus.main_r, g_bus.main_s};
  for (int c = 0; c < 3; ++c)
    for (int i = 0; i < kFrame; ++i)
      main[c][i] += (int32_t)(((int64_t)rd32(read_addr + (c * kFrame + i) * 4) * ramp[i]) >> 15);
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
  for (int m = 0; m < 4; ++m)
    for (int i = 0; i < kFrame; ++i)
      dest[m][i] += (int32_t)(((int64_t)rd32(addr[2 + m] + i * 4) * ramp[i]) >> 15);
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
  for (int i = 0; i < kFrame; ++i) {
    const uint16_t coef = rd16(table + offset + i * 2);
    g_bus.main_l[i] = (int32_t)(((int64_t)g_bus.main_l[i] * coef) >> 15);
    g_bus.main_r[i] = (int32_t)(((int64_t)g_bus.main_r[i] * coef) >> 15);
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
  for (int i = 0; i < kFrame; ++i) {
    const int16_t l = clamp16(((int64_t)g_bus.main_l[i] * ramp[i]) >> 15);
    const int16_t r = clamp16(((int64_t)g_bus.main_r[i] * ramp[i]) >> 15);
    wr16(lr_addr + i * 4, (uint16_t)r);
    wr16(lr_addr + i * 4 + 2, (uint16_t)l);
  }
}

void wm_output(const uint32_t *addr) {
  for (int m = 0; m < 4; ++m)
    for (int j = 0; j < kWmFrame; ++j)
      wr16(addr[m] + j * 2, (uint16_t)clamp16(g_bus.wm[m * 2][j]));
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

bool run_command_list(uint32_t addr, uint16_t size_words) {
  uint16_t list[512];
  const uint32_t n = std::min<uint32_t>(size_words, 512);
  for (uint32_t i = 0; i < n; ++i)
    list[i] = rd16(addr + i * 2);

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
      return produced;
    default: {
      static int warned = 0;
      if (warned++ < 8)
        std::cout << "[AX] unknown command 0x" << std::hex << cmd << " at word " << std::dec
                  << (i - 1) << "\n";
      return produced;
    }
    }
  }
  return produced;
}

}  // namespace nwii::runtime::ax::ref
