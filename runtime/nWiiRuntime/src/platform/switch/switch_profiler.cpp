#ifdef __SWITCH__
// Debug-only sampling profiler for the console (see switch_profiler.h).
//
// Each tick the sampler pauses one target thread, reads its saved user
// context and resumes it: svcSetThreadActivity(Paused) returns once the
// thread is off every core, svcGetThreadContext3 copies its registers, and
// svcSetThreadActivity(Runnable) lets it go (a few microseconds). Between pause
// and resume this thread touches no lock (no printf, no malloc): the paused
// thread may hold the stdio or heap lock. Samples go into fixed-size
// open-addressing tables allocated up front.
//
// Output, every NWII_NX_PROFILE_SECS: logs/profile_<thread>.txt with the image
// base (the elf is linked at 0, so pc - base is the elf address), cumulative
// totals since start and every (pc, lr) pair with its count, plus, for the
// CPU thread, the guest PPC pc (CPUContext::pc: the last guest call target or
// return point the generated code stored). tools/nx_profile_symbolize.py
// maps them to functions.
#include "switch_profiler.h"
#include "switch_platform.h"

#include "runtime/cpu_context.h"
#include "../../core/tprof.h"  // r31t
#include "../../core/s2.h"     // r43a: CPU de sonido

#include <switch.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace nwii::runtime {
extern CPUContext *g_ctx_ptr;  // main.cpp
extern std::atomic<uint32_t> g_guest_xfb;  // gx.cpp: game frames (XFB copies)
}

namespace nwii::runtime::nx {

namespace {

// (pc, lr) -> count. 2^18 slots (5 MB); past 3/4 full new pairs only count
// as "overflow" so probing stays short.
struct PairHist {
  static constexpr uint32_t kCap = 1u << 18;
  std::vector<uint64_t> pc, lr;
  std::vector<uint32_t> n;
  uint32_t used = 0;
  uint64_t overflow = 0;
  PairHist() : pc(kCap, 0), lr(kCap, 0), n(kCap, 0) {}
  void add(uint64_t p, uint64_t l) {
    uint32_t h = (uint32_t)(((p * 0x9E3779B97F4A7C15ull) ^ (l * 0xC2B2AE3D27D4EB4Full)) >> 46);
    for (;;) {
      h &= kCap - 1;
      if (n[h] == 0) {
        if (used >= kCap / 4 * 3) {
          ++overflow;
          return;
        }
        pc[h] = p;
        lr[h] = l;
        n[h] = 1;
        ++used;
        return;
      }
      if (pc[h] == p && lr[h] == l) {
        ++n[h];
        return;
      }
      ++h;
    }
  }
};

// guest pc -> count.
struct GuestHist {
  static constexpr uint32_t kCap = 1u << 16;
  std::vector<uint32_t> pc, n;
  uint32_t used = 0;
  uint64_t overflow = 0;
  GuestHist() : pc(kCap, 0), n(kCap, 0) {}
  void add(uint32_t p) {
    uint32_t h = (p * 0x9E3779B1u) >> 16;
    for (;;) {
      h &= kCap - 1;
      if (n[h] == 0) {
        if (used >= kCap / 4 * 3) {
          ++overflow;
          return;
        }
        pc[h] = p;
        n[h] = 1;
        ++used;
        return;
      }
      if (pc[h] == p) {
        ++n[h];
        return;
      }
      ++h;
    }
  }
};

// R16w: who is waiting. For a sample whose pc is in a kernel wait (the svc
// wrappers of svcArbitrateLock, svcWaitProcessWideKeyAtomic, svcSleepThread,
// svcWaitSynchronization), the paused thread's stack is scanned upwards from
// sp for words that point into this image's code (likely return addresses):
// the first kWaitDepth of them, with pc and lr, are counted together. Stale
// slots can show up too, but the chain above mutexLock / condvarWaitTimeout /
// nanosleep is there. Written as "wstack <count> <pc> <lr> <ret>..." lines
// (r9/r16w/tools/wstack_symbolize.py).
constexpr int kWaitDepth = 6;
struct WaitHist {
  static constexpr uint32_t kCap = 1u << 12;
  struct E { uint64_t w[kWaitDepth + 2]; uint32_t n; };
  std::vector<E> e;
  uint32_t used = 0;
  uint64_t overflow = 0;
  WaitHist() : e(kCap, E{}) {}
  void add(const uint64_t *w) {
    uint64_t h = 0;
    for (int i = 0; i < kWaitDepth + 2; ++i) h = (h ^ w[i]) * 0x9E3779B97F4A7C15ull;
    for (uint32_t k = (uint32_t)(h >> 52);; ++k) {
      E &x = e[k & (kCap - 1)];
      if (x.n == 0) {
        if (used >= kCap / 4 * 3) {
          ++overflow;
          return;
        }
        std::memcpy(x.w, w, sizeof x.w);
        x.n = 1;
        ++used;
        return;
      }
      if (std::memcmp(x.w, w, sizeof x.w) == 0) {
        ++x.n;
        return;
      }
    }
  }
};

struct Target {
  const char *name;
  int slot;
  PairHist hist;
  GuestHist guest;
  WaitHist waits;
  uint64_t stack_lo = 0, stack_hi = 0;  // mapping that held sp last time
  uint64_t samples = 0, failed = 0, paused_ticks = 0, max_pause_ticks = 0;
  uint32_t last_rc = 0;
};

uint64_t g_base = 0, g_text_end = 0;
uint64_t g_wait_fn[6] = {};
bool g_tprof = false;  // r31t: pila invitada por muestra (NWII_TPROF)
u64 g_tprof_t0 = 0;  // svc wrappers of the kernel waits
int g_core = -1, g_prio = 0;
uint32_t g_interval_us = 1000, g_write_secs = 10;
Thread g_thread;
// R16w: sampling window. NWII_NX_PROFILE_FROM / _TO: seconds since the
// profiler started (about the NRO's start); NWII_NX_PROFILE_SLOW_MS: sample
// only while the game is slow -- on once the game-frame interval (XFB copies
// of the guest, averaged over 1 s) has stayed above that many ms for
// NWII_NX_PROFILE_SLOW_HOLD_S (2) seconds, off after 1 s back under it (a
// second with no game frame at all, e.g. a load, counts as not slow). The
// two combine (AND). Outside the window nothing is sampled; the files say
// how long it was open (sampled_s) and how many times (windows).
double g_from_s = 0, g_to_s = -1, g_slow_ms = 0, g_slow_hold_s = 2;
double g_sampled_s = 0;
uint32_t g_windows = 0;

void write_target(Target &t, double elapsed) {
  const std::string path = std::string(root_dir()) + "/logs/profile_" + t.name + ".txt";
  const std::string tmp = path + ".tmp";
  FILE *f = std::fopen(tmp.c_str(), "w");
  if (!f) return;
  std::vector<uint32_t> idx;
  idx.reserve(t.hist.used);
  for (uint32_t i = 0; i < PairHist::kCap; ++i)
    if (t.hist.n[i]) idx.push_back(i);
  std::sort(idx.begin(), idx.end(), [&](uint32_t a, uint32_t b) { return t.hist.n[a] > t.hist.n[b]; });
  const double tick_us = 1e6 / 19200000.0;
  std::fprintf(f,
               "# nwii nx profile v1 (tools/nx_profile_symbolize.py)\n"
               "build " __DATE__ " " __TIME__ "\n"
               "thread %s\nbase 0x%llx\ninterval_us %u\nelapsed_s %.1f\nsamples %llu\n"
               "failed %llu (last rc 0x%x)\npaused_us_total %.0f\npaused_us_max %.1f\n"
               "overhead_pct %.2f\nsampler core %d prio 0x%X\ndistinct %u\noverflow %llu\n"
               "window from %.1f to %.1f slow_ms %.1f hold %.1f\nsampled_s %.1f\nwindows %u\n"
               "# count pc lr\n",
               t.name, (unsigned long long)g_base, g_interval_us, elapsed,
               (unsigned long long)t.samples, (unsigned long long)t.failed, t.last_rc,
               t.paused_ticks * tick_us, t.max_pause_ticks * tick_us,
               g_sampled_s > 0 ? t.paused_ticks * tick_us / (g_sampled_s * 1e4) : 0.0, g_core, g_prio,
               t.hist.used, (unsigned long long)t.hist.overflow, g_from_s, g_to_s, g_slow_ms,
               g_slow_hold_s, g_sampled_s, g_windows);
  for (uint32_t i : idx)
    std::fprintf(f, "%u 0x%llx 0x%llx\n", t.hist.n[i], (unsigned long long)t.hist.pc[i],
                 (unsigned long long)t.hist.lr[i]);
  if (t.guest.used) {
    std::vector<uint32_t> g;
    for (uint32_t i = 0; i < GuestHist::kCap; ++i)
      if (t.guest.n[i]) g.push_back(i);
    std::sort(g.begin(), g.end(), [&](uint32_t a, uint32_t b) { return t.guest.n[a] > t.guest.n[b]; });
    std::fprintf(f, "# guest count pc (CPUContext::pc, overflow %llu)\n",
                 (unsigned long long)t.guest.overflow);
    for (uint32_t i : g) std::fprintf(f, "guest %u 0x%08x\n", t.guest.n[i], t.guest.pc[i]);
  }
  if (t.waits.used) {
    std::vector<uint32_t> w;
    for (uint32_t i = 0; i < WaitHist::kCap; ++i)
      if (t.waits.e[i].n) w.push_back(i);
    std::sort(w.begin(), w.end(), [&](uint32_t a, uint32_t b) { return t.waits.e[a].n > t.waits.e[b].n; });
    std::fprintf(f, "# wait stacks: wstack count pc lr ret... (overflow %llu)\n",
                 (unsigned long long)t.waits.overflow);
    for (uint32_t i : w) {
      const WaitHist::E &x = t.waits.e[i];
      std::fprintf(f, "wstack %u", x.n);
      for (int k = 0; k < kWaitDepth + 2; ++k) std::fprintf(f, " 0x%llx", (unsigned long long)x.w[k]);
      std::fprintf(f, "\n");
    }
  }
  std::fclose(f);
  remove(path.c_str());  // the SD card's rename does not replace
  rename(tmp.c_str(), path.c_str());
}

// Paused thread in a kernel wait: count its return-address chain (see WaitHist).
int wait_stack(Target &t, const ThreadContext &ctx, uint64_t *out) {
  const uint64_t pc = ctx.pc.x;
  bool wait = false;
  for (uint64_t f : g_wait_fn) wait |= f && pc - f < 32;
  if (!wait || !g_text_end) return 0;
  const uint64_t sp = ctx.sp;
  if (sp < t.stack_lo || sp >= t.stack_hi) {
    MemoryInfo mi = {};
    u32 pi = 0;
    if (R_FAILED(svcQueryMemory(&mi, &pi, sp)) || !(mi.perm & Perm_R)) return 0;
    t.stack_lo = mi.addr;
    t.stack_hi = mi.addr + mi.size;
  }
  uint64_t w[kWaitDepth + 2] = {pc, ctx.lr};
  int k = 2;
  const uint64_t end = std::min<uint64_t>(t.stack_hi, sp + 1024);
  for (uint64_t a = sp & ~7ull; a + 8 <= end && k < kWaitDepth + 2; a += 8) {
    const uint64_t v = *(const volatile uint64_t *)a;
    if (v >= g_base && v < g_text_end && !(v & 3) && v != w[k - 1]) w[k++] = v;
  }
  t.waits.add(w);
  for (int i = 2; i < k; ++i) out[i - 2] = w[i];
  return k - 2;
}

// Returns false when the kernel refuses (e.g. an emulator without the svc).
bool sample(Target &t) {
  const Handle h = (Handle)thread_handle(t.slot);
  if (!h) return true;
  ThreadContext ctx;
  const u64 t0 = armGetSystemTick();
  Result rc = svcSetThreadActivity(h, ThreadActivity_Paused);
  Result rc2 = rc;
  if (R_SUCCEEDED(rc)) {
    rc2 = svcGetThreadContext3(&ctx, h);
    if (R_SUCCEEDED(rc2)) {
      uint64_t hret[kWaitDepth];
      const int nh = wait_stack(t, ctx, hret);  // while the stack holds still
      if (g_tprof && t.slot == kThreadCpu) {
        if (CPUContext *c = g_ctx_ptr)
          tprof::sample(*c, ctx.pc.x, ctx.lr, (uint32_t)(armTicksToNs(t0 - g_tprof_t0) / 1000), hret, nh);
      } else if (g_tprof) {
        tprof::sample_host(t.slot, ctx.pc.x, ctx.lr, (uint32_t)(armTicksToNs(t0 - g_tprof_t0) / 1000), hret, nh);
      }
    }
    svcSetThreadActivity(h, ThreadActivity_Runnable);
  }
  const u64 dt = armGetSystemTick() - t0;
  if (R_FAILED(rc2)) {
    ++t.failed;
    t.last_rc = rc2;
    return t.samples > 0 || t.failed < 50;  // never worked: give up
  }
  t.paused_ticks += dt;
  t.max_pause_ticks = std::max<uint64_t>(t.max_pause_ticks, dt);
  ++t.samples;
  t.hist.add(ctx.pc.x, ctx.lr);
  if (t.slot == kThreadCpu)
    if (CPUContext *c = g_ctx_ptr) t.guest.add(*(volatile uint32_t *)&c->pc);
  if (t.slot == kThreadS2)  // r43a
    if (CPUContext *c = s2::snd_ctx()) t.guest.add(*(volatile uint32_t *)&c->pc);
  return true;
}

void sampler(void *) {
  // R11: the render worker too (logs/profile_gxw.txt; no samples while it is off).
  // r43a: NWII_A_PROF_SND=1 muestrea tambien la CPU de sonido (S2) -> logs/profile_snd.txt (con pc invitado).
  static Target targets[] = {{"main", kThreadMain}, {"cpu", kThreadCpu}, {"gxw", kThreadGxw}, {"snd", kThreadS2}};
  static const int ntargets = [] { const char *e = std::getenv("NWII_A_PROF_SND"); return (e && *e && *e != '0') ? 4 : 3; }();
  const u64 start = armGetSystemTick();
  u64 last_write = start;
  uint64_t win_paused[3] = {};
  const bool windowed = g_from_s > 0 || g_to_s >= 0 || g_slow_ms > 0;
  bool active = false;
  u64 last_tick = start, slow_check = start;
  uint32_t xfb_hist[4] = {};  // game frames at the last 4 quarter-second checks
  int xfb_n = 0;
  double slow_for = 0, fast_for = 0;
  bool slow = false;
  for (;;) {
    svcSleepThread((s64)g_interval_us * 1000);
    const u64 tnow = armGetSystemTick();
    const double now_s = armTicksToNs(tnow - start) / 1e9;
    if (g_slow_ms > 0 && armTicksToNs(tnow - slow_check) >= 250000000ull) {
      // Mean game-frame interval over the last second.
      const double dt = armTicksToNs(tnow - slow_check) / 1e9;
      slow_check = tnow;
      const uint32_t x = g_guest_xfb.load(std::memory_order_relaxed);
      const uint32_t old = xfb_hist[xfb_n & 3];
      xfb_hist[xfb_n & 3] = x;
      if (++xfb_n > 4) {
        const uint32_t d = x - old;
        const bool s_now = d > 0 && 1000.0 / d > g_slow_ms;  // 1 s / frames, in ms
        if (s_now) { slow_for += dt; fast_for = 0; }
        else { fast_for += dt; slow_for = 0; }
        if (!slow && slow_for >= g_slow_hold_s) slow = true;
        if (slow && fast_for >= 1.0) slow = false;
      }
    }
    const bool want = !windowed || ((now_s >= g_from_s) && (g_to_s < 0 || now_s < g_to_s) &&
                                    (g_slow_ms <= 0 || slow));
    if (want != active) {
      active = want;
      if (active) ++g_windows;
      std::printf("[PROFILE] window %s at %.1f s (sampled so far %.1f s)\n", active ? "open" : "closed",
                  now_s, g_sampled_s);
      if (!active) {  // the files right away: the game may be closed during the next one
        mkdir((std::string(root_dir()) + "/logs").c_str(), 0777);
        for (int k = 0; k < ntargets; ++k) write_target(targets[k], now_s);
      }
    }
    if (active) g_sampled_s += armTicksToNs(tnow - last_tick) / 1e9;
    last_tick = tnow;
    if (!active) continue;
    for (int k = 0; k < ntargets; ++k) {
      Target &t = targets[k];
      if (sample(t)) continue;
      std::printf("[PROFILE] %s: svcSetThreadActivity/svcGetThreadContext3 failed (rc 0x%x): "
                  "sampling is not supported here, profiler off\n", t.name, t.last_rc);
      std::fflush(stdout);
      return;
    }
    const u64 now = armGetSystemTick();
    if (armTicksToNs(now - last_write) >= (u64)g_write_secs * 1000000000ull) {
      last_write = now;
      const double elapsed = armTicksToNs(now - start) / 1e9;
      mkdir((std::string(root_dir()) + "/logs").c_str(), 0777);
      for (int k = 0; k < ntargets; ++k) write_target(targets[k], elapsed);
      if (g_tprof) tprof::flush();
      const double tick_us = 1e6 / 19200000.0;
      std::printf("[PROFILE] %.0f s: main %llu samples (%.2f %% paused), cpu %llu (%.2f %%) -> "
                  "logs/profile_*.txt\n", elapsed,
                  (unsigned long long)targets[0].samples,
                  targets[0].paused_ticks * tick_us / (elapsed * 1e4),
                  (unsigned long long)targets[1].samples,
                  targets[1].paused_ticks * tick_us / (elapsed * 1e4));
      // Keep the cost under ~2 %: if a thread spent more than 1.5 % of the
      // last window paused, sample half as often (up to every 8 ms). The
      // histograms are proportions, so they stay comparable.
      double worst = 0;
      for (int i = 0; i < 3; ++i) {
        const uint64_t d = targets[i].paused_ticks - win_paused[i];
        win_paused[i] = targets[i].paused_ticks;
        worst = std::max(worst, d * tick_us / (g_write_secs * 1e4));
      }
      if (worst > 1.5 && g_interval_us < 8000) {
        g_interval_us *= 2;
        std::printf("[PROFILE] %.2f %% paused in the last %u s: now every %u us\n", worst,
                    g_write_secs, g_interval_us);
      }
    }
  }
}

}  // namespace

void profiler_start() {
  const char *e = std::getenv("NWII_NX_PROFILE");
  if (!e || !*e || !std::strcmp(e, "0")) return;
  if (const char *v = std::getenv("NWII_NX_PROFILE_US")) g_interval_us = std::max(100, std::atoi(v));
  if (const char *v = std::getenv("NWII_NX_PROFILE_SECS")) g_write_secs = std::max(1, std::atoi(v));
  if (const char *v = std::getenv("NWII_NX_PROFILE_FROM")) g_from_s = std::atof(v);
  if (const char *v = std::getenv("NWII_NX_PROFILE_TO")) g_to_s = std::atof(v);
  if (const char *v = std::getenv("NWII_NX_PROFILE_SLOW_MS")) g_slow_ms = std::atof(v);
  if (const char *v = std::getenv("NWII_NX_PROFILE_SLOW_HOLD_S")) g_slow_hold_s = std::atof(v);
  if (g_from_s > 0 || g_to_s >= 0 || g_slow_ms > 0)
    std::printf("[PROFILE] window: from %.1f s to %.1f s (-1: end), slow scenes only: %s "
                "(game frame > %.1f ms for %.1f s)\n", g_from_s, g_to_s, g_slow_ms > 0 ? "yes" : "no",
                g_slow_ms, g_slow_hold_s);
  MemoryInfo region = {};
  u32 page_info = 0;
  svcQueryMemory(&region, &page_info, (u64)(uintptr_t)&profiler_start);
  g_base = region.addr;  // start of the NRO's code (elf address 0)
  g_text_end = region.addr + region.size;
  g_wait_fn[0] = (uint64_t)(uintptr_t)&svcArbitrateLock;
  g_wait_fn[1] = (uint64_t)(uintptr_t)&svcWaitProcessWideKeyAtomic;
  g_wait_fn[2] = (uint64_t)(uintptr_t)&svcSleepThread;
  g_wait_fn[3] = (uint64_t)(uintptr_t)&svcWaitSynchronization;
  g_wait_fn[4] = (uint64_t)(uintptr_t)&svcSendSyncRequest;
  g_wait_fn[5] = (uint64_t)(uintptr_t)&svcWaitForAddress;
  mkdir((std::string(root_dir()) + "/logs").c_str(), 0777);
  g_tprof_t0 = armGetSystemTick();
  g_tprof = tprof::init((std::string(root_dir()) + "/logs").c_str(), g_base, __DATE__ " " __TIME__);
  // Core 3 if this process may use it (homebrew normally gets 0-2), else 2
  // (the audio cores: AX mixer + audio output, a few % busy).
  u64 allowed = 0;
  svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  g_core = (allowed & 8) ? 3 : (allowed & 4) ? 2 : -2;
  g_prio = 0x2C;
  {  // r43a: para muestrear la CPU de sonido (0x2C, nucleo 2) el muestreador tiene que estar por encima de ella
    const char *e = std::getenv("NWII_A_PROF_SND");
    if (e && *e && *e != '0') g_prio = 0x2B;
  }
  Result rc = threadCreate(&g_thread, sampler, nullptr, nullptr, 64 * 1024, g_prio, g_core);
  if (R_SUCCEEDED(rc)) rc = threadStart(&g_thread);
  std::printf("[PROFILE] sampler %s: every %u us on core %d (allowed mask 0x%llx), image base "
              "0x%llx, writes logs/profile_{main,cpu}.txt every %u s\n",
              R_SUCCEEDED(rc) ? "started" : "failed to start", g_interval_us, g_core,
              (unsigned long long)allowed, (unsigned long long)g_base, g_write_secs);
}

}  // namespace nwii::runtime::nx
#endif  // __SWITCH__
