#ifdef __SWITCH__
// Native Switch audio output: libnx audren + audrv, one voice of 32 kHz
// stereo s16 (what the DSP HLE plays, hw_dsp.cpp), four 8 ms wave buffers.
//
// Why not SDL: devkitPro's SDL2 2.28.5 Switch driver queues a buffer and then
// spins until that buffer is "Playing". libnx can move a buffer from Queued
// straight to Done when the audio thread is late by more than a buffer, and
// then the loop never ends: all audio stops for good (seen in another port on
// hardware). SDL's audio thread also runs at the lowest priority, and here the
// main thread it shares core 0 with is saturated. And the old feed was the
// host loop (~10/s on hardware), so audio jittered with the render rate.
//
// This thread (core 2, next to the AX mixer, priority 0x2B) wakes on every
// audio renderer frame (5 ms, or a 10 ms timeout), refills any finished
// buffer from the DSP output ring and never waits for a particular buffer
// state. Policy, as with the SDL queue: after an underrun caused by an empty
// ring it waits for 64 ms of audio before playing again; above 250 ms queued
// (ring + device) it drops the oldest frames down to 128 ms, so latency cannot
// grow without bound. If the voice stopped with buffers queued it is
// restarted; if no sample plays for 1 s with buffers queued (renderer stalled,
// e.g. after HOME / sleep) the voice is reset (watchdog).
#include "switch_audio.h"
#include "switch_platform.h"

#include "runtime/hw/hw.h"

#include <switch.h>
#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nwii::runtime::nx {

namespace {

constexpr int kRate = 32000;
constexpr int kBufFrames = 256;  // 8 ms per wave buffer
constexpr int kNumBufs = 4;      // 32 ms in the renderer at most
constexpr size_t kPoolBytes = (kNumBufs * kBufFrames * 4 + 0xFFF) & ~size_t(0xFFF);
// Defaults as with the SDL queue; debug.toml NWII_NX_AUDIO_PRIME_MS /
// NWII_NX_AUDIO_MAX_MS change them for tests on the console.
size_t g_prime_frames = 2048;       // 64 ms before (re)starting
size_t g_max_queued_frames = 8000;  // 250 ms: above it, drop ...
size_t g_drop_to_frames = 4096;     // ... the oldest down to 128 ms (or the prime level)
constexpr uint64_t kWatchdogNs = 1000000000ull;

const AudioRendererConfig kConfig = {
    .output_rate = AudioRendererOutputRate_48kHz,
    .num_voices = 2,
    .num_effects = 0,
    .num_sinks = 1,
    .num_mix_objs = 1,
    .num_mix_buffers = 2,
};

struct Out {
  AudioDriver drv;
  AudioDriverWaveBuf wb[kNumBufs];
  int16_t *pool = nullptr;
  Thread thread;
  bool renderer = false, driver = false, running = false;
  std::atomic<bool> quit{false};
  int prio = 0, core = -1;
  // Counters (written by the audio thread, read by fps_log).
  std::atomic<uint64_t> frames_out{0}, frames_dropped{0};
  std::atomic<uint32_t> underruns{0}, starves{0}, restarts{0}, watchdog{0};
  std::atomic<uint32_t> q_now{0}, q_min{UINT32_MAX}, q_max{0}, ring_now{0};
  std::atomic<uint64_t> gap_max_ns{0};
};
Out *g_out = nullptr;

inline uint64_t now_ns() { return armTicksToNs(armGetSystemTick()); }

inline bool in_flight(const AudioDriverWaveBuf &b) {
  return b.state == AudioDriverWaveBufState_Waiting || b.state == AudioDriverWaveBufState_Queued ||
         b.state == AudioDriverWaveBufState_Playing;
}

void atomic_max(std::atomic<uint32_t> &a, uint32_t v) {
  uint32_t cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}
void atomic_min(std::atomic<uint32_t> &a, uint32_t v) {
  uint32_t cur = a.load(std::memory_order_relaxed);
  while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

void audio_thread(void *) {
  Out &o = *g_out;
  note_thread(kThreadAudio);
  static int16_t scratch[1024 * 2];  // dropped frames land here
  bool priming = true, played = false;
  u32 last_played = 0;
  uint64_t last_progress = now_ns(), last_wake = now_ns();
  Event *frame_ev = audrenGetFrameEvent();

  while (!o.quit.load(std::memory_order_relaxed)) {
    // One audio renderer frame is 5 ms; the timeout keeps the thread alive if
    // the renderer stops signalling (suspended, or an emulator without it).
    if (frame_ev) eventWait(frame_ev, 10'000'000ull);
    else svcSleepThread(5'000'000ll);
    const uint64_t t = now_ns();
    const uint64_t gap = t - last_wake;
    last_wake = t;
    if (gap > o.gap_max_ns.load(std::memory_order_relaxed))
      o.gap_max_ns.store(gap, std::memory_order_relaxed);

    audrvUpdate(&o.drv);
    int inflight = 0;
    for (const AudioDriverWaveBuf &b : o.wb) inflight += in_flight(b);

    // Watchdog: buffers queued but nothing played for 1 s.
    const u32 pc = audrvVoiceGetPlayedSampleCount(&o.drv, 0);
    if (pc != last_played || inflight == 0) {
      last_played = pc;
      last_progress = t;
    } else if (t - last_progress > kWatchdogNs) {
      o.watchdog.fetch_add(1, std::memory_order_relaxed);
      audrvVoiceStop(&o.drv, 0);  // flags every queued buffer Done
      audrvVoiceStart(&o.drv, 0);
      audrvUpdate(&o.drv);
      last_progress = t;
      priming = true;
      continue;
    }

    size_t avail = hw::dsp_audio_available();
    if (played && inflight == 0) {
      // The device ran dry. With data in the ring this thread was late (the
      // ring refills it at once); with an empty ring the game was.
      o.underruns.fetch_add(1, std::memory_order_relaxed);
      if (avail < (size_t)kBufFrames) {
        o.starves.fetch_add(1, std::memory_order_relaxed);
        priming = true;
      }
      played = false;
    }

    // Latency cap: drop the oldest frames of the ring.
    const size_t dev = (size_t)inflight * kBufFrames;
    if (avail + dev > g_max_queued_frames) {
      size_t drop = avail + dev - std::max(g_drop_to_frames, dev);
      drop = std::min(drop, avail);
      o.frames_dropped.fetch_add(drop, std::memory_order_relaxed);
      while (drop) {
        const size_t n = hw::dsp_audio_pull(scratch, std::min<size_t>(drop, 1024));
        if (!n) break;
        drop -= n;
      }
      avail = hw::dsp_audio_available();
    }

    if (priming) {
      if (avail < g_prime_frames) {
        o.ring_now.store((uint32_t)avail, std::memory_order_relaxed);
        o.q_now.store((uint32_t)(avail + (size_t)inflight * kBufFrames), std::memory_order_relaxed);
        continue;
      }
      priming = false;
    }

    // Refill every finished buffer while full buffers are available.
    bool queued_any = false;
    for (int i = 0; i < kNumBufs && avail >= (size_t)kBufFrames; ++i) {
      AudioDriverWaveBuf &b = o.wb[i];
      if (in_flight(b)) continue;
      int16_t *dst = o.pool + (size_t)i * kBufFrames * 2;
      const size_t n = hw::dsp_audio_pull(dst, kBufFrames);
      if (n < (size_t)kBufFrames)
        std::memset(dst + n * 2, 0, (kBufFrames - n) * 4);
      armDCacheFlush(dst, kBufFrames * 4);
      audrvVoiceAddWaveBuf(&o.drv, 0, &b);
      o.frames_out.fetch_add(kBufFrames, std::memory_order_relaxed);
      avail -= std::min(avail, n);
      ++inflight;
      queued_any = true;
    }
    if (queued_any) {
      played = true;
      if (!audrvVoiceIsPlaying(&o.drv, 0)) {
        o.restarts.fetch_add(1, std::memory_order_relaxed);
        audrvVoiceStart(&o.drv, 0);
      }
      audrvUpdate(&o.drv);
    }
    const uint32_t q = (uint32_t)(avail + (size_t)inflight * kBufFrames);
    o.q_now.store(q, std::memory_order_relaxed);
    o.ring_now.store((uint32_t)avail, std::memory_order_relaxed);
    atomic_min(o.q_min, q);
    atomic_max(o.q_max, q);
  }
}

void close_out(Out &o) {
  if (o.driver) audrvClose(&o.drv);
  if (o.renderer) audrenExit();
  o.driver = o.renderer = false;
  if (o.pool) std::free(o.pool);
  o.pool = nullptr;
}

}  // namespace

bool audio_start() {
  if (g_out) return g_out->running;
  const char *mode = std::getenv("NWII_NX_AUDIO");
  if (mode && (!std::strcmp(mode, "sdl") || !std::strcmp(mode, "SDL"))) {
    std::printf("[NX] audio: SDL device (NWII_NX_AUDIO=sdl)\n");
    return false;
  }
  auto env_ms = [](const char *name, size_t def) {
    const char *v = std::getenv(name);
    const int ms = v ? std::atoi(v) : 0;
    return ms > 0 ? std::min<size_t>((size_t)ms * kRate / 1000, 1u << 14) : def;
  };
  g_prime_frames = env_ms("NWII_NX_AUDIO_PRIME_MS", g_prime_frames);
  g_max_queued_frames = std::max(env_ms("NWII_NX_AUDIO_MAX_MS", g_max_queued_frames),
                                 g_prime_frames + kNumBufs * kBufFrames);
  g_drop_to_frames = std::max(g_drop_to_frames, g_prime_frames);
  if (g_drop_to_frames >= g_max_queued_frames) g_drop_to_frames = g_max_queued_frames / 2;
  g_out = new Out;
  Out &o = *g_out;
  Result rc = audrenInitialize(&kConfig);
  if (R_SUCCEEDED(rc)) {
    o.renderer = true;
    rc = audrvCreate(&o.drv, &kConfig, 2);
  }
  if (R_SUCCEEDED(rc)) {
    o.driver = true;
    o.pool = (int16_t *)memalign(0x1000, kPoolBytes);
    if (!o.pool) rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
  }
  if (R_SUCCEEDED(rc)) {
    std::memset(o.pool, 0, kPoolBytes);
    armDCacheFlush(o.pool, kPoolBytes);
    for (int i = 0; i < kNumBufs; ++i) {
      AudioDriverWaveBuf &b = o.wb[i];
      std::memset(&b, 0, sizeof b);
      b.data_raw = o.pool;
      b.size = kPoolBytes;
      b.start_sample_offset = i * kBufFrames;
      b.end_sample_offset = (i + 1) * kBufFrames;
    }
    const int mp = audrvMemPoolAdd(&o.drv, o.pool, kPoolBytes);
    static const u8 sink_channels[] = {0, 1};
    if (mp < 0 || !audrvMemPoolAttach(&o.drv, mp) ||
        audrvDeviceSinkAdd(&o.drv, AUDREN_DEFAULT_DEVICE_NAME, 2, sink_channels) < 0)
      rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
  }
  if (R_SUCCEEDED(rc)) rc = audrvUpdate(&o.drv);
  if (R_SUCCEEDED(rc)) rc = audrenStartAudioRenderer();
  if (R_SUCCEEDED(rc) && !audrvVoiceInit(&o.drv, 0, 2, PcmFormat_Int16, kRate))
    rc = MAKERESULT(Module_Libnx, LibnxError_BadInput);
  if (R_SUCCEEDED(rc)) {
    audrvVoiceSetDestinationMix(&o.drv, 0, AUDREN_FINAL_MIX_ID);
    audrvVoiceSetMixFactor(&o.drv, 0, 1.0f, 0, 0);
    audrvVoiceSetMixFactor(&o.drv, 0, 0.0f, 0, 1);
    audrvVoiceSetMixFactor(&o.drv, 0, 0.0f, 1, 0);
    audrvVoiceSetMixFactor(&o.drv, 0, 1.0f, 1, 1);
    audrvVoiceStart(&o.drv, 0);
    rc = audrvUpdate(&o.drv);
  }
  if (R_FAILED(rc)) {
    std::printf("[NX] audio: audren setup failed (0x%x), using the SDL device\n", (unsigned)rc);
    close_out(o);
    return false;
  }

  // Core 2 (the AX mixer's, ~3 % busy) at 0x2B: above the default 0x2C of
  // every other thread of the process, so a busy core never delays a refill.
  u64 allowed = 0;
  svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  o.core = (allowed & 4) ? 2 : -2;
  for (int prio : {0x2B, 0x2C}) {
    rc = threadCreate(&o.thread, audio_thread, nullptr, nullptr, 64 * 1024, prio, o.core);
    if (R_SUCCEEDED(rc)) {
      o.prio = prio;
      break;
    }
  }
  if (R_SUCCEEDED(rc)) rc = threadStart(&o.thread);
  if (R_FAILED(rc)) {
    std::printf("[NX] audio: thread start failed (0x%x), using the SDL device\n", (unsigned)rc);
    close_out(o);
    return false;
  }
  o.running = true;
  std::printf("[NX] audio: native audren output, %d Hz stereo, %d x %d ms buffers, prime %zu ms, "
              "max %zu ms, thread priority 0x%X on core %d (NWII_NX_AUDIO=sdl for the SDL "
              "device)\n",
              kRate, kNumBufs, kBufFrames * 1000 / kRate, g_prime_frames * 1000 / kRate,
              g_max_queued_frames * 1000 / kRate, o.prio, o.core);
  return true;
}

void audio_stop() {
  if (!g_out || !g_out->running) return;
  Out &o = *g_out;
  o.quit.store(true);
  threadWaitForExit(&o.thread);
  threadClose(&o.thread);
  o.running = false;
  audrvVoiceStop(&o.drv, 0);
  audrvUpdate(&o.drv);
  audrenStopAudioRenderer();
  close_out(o);
}

void audio_report(char *buf, size_t n, double dt) {
  buf[0] = 0;
  if (!g_out || !g_out->running || n == 0) return;
  Out &o = *g_out;
  static uint64_t last_out = 0, last_drop = 0;
  static uint32_t last_under = 0;
  const uint64_t out = o.frames_out.load(), drop = o.frames_dropped.load();
  const uint32_t under = o.underruns.load();
  const uint32_t qmin = o.q_min.exchange(UINT32_MAX), qmax = o.q_max.exchange(0);
  const uint64_t gap = o.gap_max_ns.exchange(0);
  auto ms = [](uint32_t frames) { return frames * 1000.0 / kRate; };
  std::snprintf(buf, n,
                ", audio nx q %.0f ms (%.0f-%.0f), ring %.0f ms, out %.1f kHz, underruns %u (+%u, "
                "%u starved), restarts %u, watchdog %u, dropped %.0f ms (+%.0f), gap max %.1f ms",
                ms(o.q_now.load()), qmin == UINT32_MAX ? 0.0 : ms(qmin), ms(qmax),
                ms(o.ring_now.load()), dt > 0 ? (out - last_out) / dt / 1000.0 : 0.0, under,
                under - last_under, o.starves.load(), o.restarts.load(), o.watchdog.load(),
                drop * 1000.0 / kRate, (drop - last_drop) * 1000.0 / kRate, gap / 1e6);
  last_out = out;
  last_drop = drop;
  last_under = under;
}

}  // namespace nwii::runtime::nx
#endif  // __SWITCH__
