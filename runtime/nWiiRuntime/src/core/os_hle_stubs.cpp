#include <atomic>
#include "s2.h"  // r34s2
#include "runtime/nwii_env.h"
#include <cstdint>
void vtl_note(uint32_t kind, uint32_t arg);  // r37v (gx.cpp): NWII_V_TL
void vtl_idle_turn(bool at_end);             // r37v (gx.cpp)
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include "runtime/hw/hw.h"
#include "runtime/hw/mmio_dispatcher.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <algorithm>
#include "runtime/event_scheduler.h"
#include "../hle/ax/ax_hle.h"
#ifdef __SWITCH__
#include <switch.h>
#endif

using namespace nwii::runtime;

namespace nwii::runtime {
uint64_t get_os_time();
}

uint64_t nwii::runtime::get_os_time() {
  auto now = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(
             now.time_since_epoch())
      .count();
}

namespace nwii::runtime {
bool g_trace_calls = []() {
  const char *env = NWII_ENV("NWII_TRACE_CALLS");
  return env && env[0] == '1';
}();

uint32_t g_watch_addr = []() -> uint32_t {
  const char *env = NWII_ENV("NWII_WATCH");
  return env ? (uint32_t)std::strtoul(env, nullptr, 16) : 0;
}();

extern CPUContext *g_ctx_ptr;

void watch_hit(uint32_t addr, uint32_t value, int width) {
  CPUContext *c = g_ctx_ptr;
  std::cout << "[Watch] write" << width << " 0x" << std::hex << addr << " = 0x"
            << value;
  if (c) {
    std::cout << " pc=0x" << c->pc << " lr=0x" << c->lr;

    
    static const bool regs = NWII_ENV("NWII_WATCH_REGS") != nullptr;
    if (regs) {
      for (int r = 24; r <= 31; ++r)
        std::cout << " r" << std::dec << r << "=0x" << std::hex << c->gpr[r];
      std::cout << " r3=0x" << c->gpr[3] << " r4=0x" << c->gpr[4];
    }
  }
  std::cout << std::dec << "\n";
}


static std::set<uint32_t> g_trace_only = []() {
  std::set<uint32_t> s;
  const char *env = NWII_ENV("NWII_TRACE_ONLY");
  if (env) {
    std::string in(env);
    size_t p = 0;
    while (p < in.size()) {
      size_t c = in.find(',', p);
      if (c == std::string::npos) c = in.size();
      s.insert((uint32_t)std::strtoul(in.substr(p, c - p).c_str(), nullptr, 16));
      p = c + 1;
    }
  }
  return s;
}();

// r38c: contador atomico (antes `static int` sin proteger: D07 escribio 928 917 lineas hasta llenar el registro);
// 20 lineas y luego solo el contador, que el vigilante (s2.cpp) vigila como alarma.
std::atomic<uint64_t> g_null_calls{0};
std::atomic<uint32_t> g_null_last_site{0};
void note_null_call(uint32_t site, uint32_t lr) {
  const uint64_t n = g_null_calls.fetch_add(1, std::memory_order_relaxed);
  g_null_last_site.store(site, std::memory_order_relaxed);
  if (n < 20)
    std::printf("[NullCall] no-op call through null ptr at site 0x%x lr=0x%x\n", site, lr);
  else if (n == 20)
    std::printf("[NullCall] (mas avisos omitidos; el vigilante da el total)\n");
}

void trace_call(uint32_t func_addr, CPUContext &ctx) {
  if (!g_trace_only.empty() && !g_trace_only.count(func_addr)) return;
  std::cout << "[CALL] 0x" << std::hex << func_addr << " lr=0x" << ctx.lr
            << " r3=0x" << ctx.gpr[3] << " r4=0x" << ctx.gpr[4] << " r5=0x"
            << ctx.gpr[5] << " r6=0x" << ctx.gpr[6] << " r7=0x" << ctx.gpr[7]
            << " r13=0x" << ctx.gpr[13] << " r1=0x" << ctx.gpr[1]
            << std::dec << "\n";
}
} 

static std::string read_guest_string(CPUContext &ctx, uint32_t addr) {
  std::string str;
  while (true) {
    uint8_t c = ctx.mmu.read8(addr++);
    if (c == 0)
      break;
    str += (char)c;
  }
  return str;
}

extern "C" void OSReport(CPUContext &ctx) {
  uint32_t format_addr = ctx.gpr[3];
  std::string format_str = read_guest_string(ctx, format_addr);

  std::string result;
  int arg_idx = 4;
  int fpr_idx = 1;

  for (size_t i = 0; i < format_str.length(); ++i) {
    if (format_str[i] == '%') {
      if (i + 1 < format_str.length() && format_str[i + 1] == '%') {
        result += '%';
        i++;
        continue;
      }

      std::string specifier = "%";
      i++;
      while (i < format_str.length() &&
             std::string("0123456789.-+ #hlz").find(format_str[i]) !=
                 std::string::npos) {
        specifier += format_str[i];
        i++;
      }

      if (i < format_str.length()) {
        char type = format_str[i];
        specifier += type;

        if (type == 'd' || type == 'i' || type == 'c') {
          char buf[64];
          std::snprintf(buf, sizeof(buf), specifier.c_str(),
                        (int32_t)ctx.gpr[arg_idx++]);
          result += buf;
        } else if (type == 'u' || type == 'x' || type == 'X' || type == 'p') {
          char buf[64];
          std::snprintf(buf, sizeof(buf), specifier.c_str(),
                        (uint32_t)ctx.gpr[arg_idx++]);
          result += buf;
        } else if (type == 's') {
          uint32_t str_ptr = ctx.gpr[arg_idx++];
          if (str_ptr != 0)
            result += read_guest_string(ctx, str_ptr);
          else
            result += "(null)";
        } else if (type == 'f') {
          char buf[64];
          if (fpr_idx <= 8)
            std::snprintf(buf, sizeof(buf), specifier.c_str(),
                          ctx.fpr[fpr_idx++]);
          else
            std::snprintf(buf, sizeof(buf), "[float_spilled]");
          result += buf;
        } else {
          result += specifier;
        }
      }
    } else {
      result += format_str[i];
    }
  }
  std::cout << "[OSReport] [LR:0x" << std::hex << ctx.lr << std::dec << "] " << result;
}

namespace nwii::runtime {
uint32_t HW_Reg_Read32(uint32_t addr) {
  if ((addr & 0xFF000000) != 0xCD000000 && (addr & 0xFF000000) != 0x0D000000) {
    addr = (addr & 0x00FFFFFF) | 0xCC000000;
  }
  return MMIODispatcher::get().read32(addr);
}
uint16_t HW_Reg_Read16(uint32_t addr) {
  if ((addr & 0xFF000000) != 0xCD000000 && (addr & 0xFF000000) != 0x0D000000) {
    addr = (addr & 0x00FFFFFF) | 0xCC000000;
  }
  return MMIODispatcher::get().read16(addr);
}
void HW_Reg_Write32(uint32_t addr, uint32_t val) {
  if ((addr & 0xFF000000) != 0xCD000000 && (addr & 0xFF000000) != 0x0D000000) {
    addr = (addr & 0x00FFFFFF) | 0xCC000000;
  }
  MMIODispatcher::get().write32(addr, val);
}
void HW_Reg_Write16(uint32_t addr, uint16_t val) {
  if ((addr & 0xFF000000) != 0xCD000000 && (addr & 0xFF000000) != 0x0D000000) {
    addr = (addr & 0x00FFFFFF) | 0xCC000000;
  }
  MMIODispatcher::get().write16(addr, val);
}
} 

extern "C" void DVD_Callback(CPUContext &ctx) {
  std::cout << "[HLE DVD_Callback] Triggered at PC=0x" << std::hex << ctx.pc
            << std::dec << "\n";
}

extern "C" void VIInit(CPUContext &ctx) {
  std::cout << "[HLE VIInit] Triggered at PC=0x" << std::hex << ctx.pc
            << std::dec << "\n";
  
}

extern "C" void VIConfigure(CPUContext &ctx) {
  std::cout << "[HLE VIConfigure] Triggered at PC=0x" << std::hex << ctx.pc
            << std::dec << "\n";
}

extern "C" void VIConfigurePan(CPUContext &ctx) {
  std::cout << "[HLE VIConfigurePan] Triggered at PC=0x" << std::hex << ctx.pc
            << std::dec << "\n";
}

extern "C" void VISetBlack(CPUContext &ctx) {
  std::cout << "[HLE VISetBlack] Triggered at PC=0x" << std::hex << ctx.pc
            << std::dec << "\n";
}

extern "C" void VIGetNextField(CPUContext &ctx) {
  static uint32_t field = 0;
  field ^= 1;
  ctx.gpr[3] = field;
}

extern "C" void VIWaitForRetrace(CPUContext &ctx) {

  nwii::runtime::hw::vi_trigger_interrupt();
}
namespace nwii::runtime {

NativeStackGuard g_native_stack = {0, 0};  // R9 guestloop (cpu_context.h)

extern int g_ipc_interrupt_delay;
extern std::atomic<uint32_t> g_v_dispatch_seq;  // r37v (ios.cpp)

// Time the CPU thread spent asleep in idle_wait (NWII_FPS=1 reports it).
std::atomic<uint64_t> g_idle_sleeps{0}, g_idle_sleep_us{0};

// ---- R9 guestloop: host clock -> guest timebase -----------------------------
// r8: steady_clock::now() in whole microseconds (std::chrono, then two
// divisions). On the Switch steady_clock is clock_gettime(CLOCK_MONOTONIC)
// from libnx, which reads the 19.2 MHz system tick and converts it to ns.
// NWII_TB_SRC=tick (Switch default) reads the tick directly: 40.5 MHz =
// 19.2 MHz x 135/64 exactly, so tb = ticks * 135 / 64 (finer than 1 us).
// NWII_TB_SRC=chrono keeps the r8 path. The tick clock continues the r8 one
// from its first read (same origin, tb_start).
// NWII_TB_BENCH=1 prints the cost of each clock read once at start-up.
static uint64_t chrono_timebase(const CPUContext &c) {
  uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - c.tb_start)
                    .count();
  return us / 1000000 * c.tb_freq + us % 1000000 * c.tb_freq / 1000000;
}
#ifdef __SWITCH__
// Continues the r8 clock: at the first read, guest time = chrono_timebase()
// and the tick count is taken as the base; afterwards only ticks are read.
// r10a fix (8-nov, real Switch hang): the origin is taken ONCE, in main()
// before any other thread exists (CPUContext::wall_timebase_now() is called
// there right after tb_start is set). Before, the first reader set it lazily:
// another thread (render/audio/AX) that had read the tick counter just before
// the origin was published got a negative difference, i.e. a huge unsigned
// one, tb_cached jumped ~2^60 ticks ahead and, since the guest clock never goes
// back, it froze for good (black screen, CPU thread at 100 %, busy-wait at
// 0x801ABFE0 never ending). Now a tick older than the origin counts as 0.
static uint64_t g_tick0 = 0, g_tick_tb0 = 0;
static uint64_t tick_timebase(const CPUContext &c) {
  if (__builtin_expect(g_tick0 == 0, 0)) {
    g_tick_tb0 = chrono_timebase(c);
    g_tick0 = armGetSystemTick() | 1;
  }
  const uint64_t t = armGetSystemTick();
  const uint64_t d = t > g_tick0 ? t - g_tick0 : 0;
  if (c.tb_freq == 40500000) return g_tick_tb0 + d / 64 * 135 + (d % 64) * 135 / 64;
  return g_tick_tb0 + d / 19200000 * c.tb_freq + (d % 19200000) * c.tb_freq / 19200000;
}
#endif
static void tb_bench(const CPUContext &c) {
  using clk = std::chrono::steady_clock;
  volatile uint64_t sink = 0;
  auto run = [&](const char *name, auto fn) {
    const int n = 200000;
    const auto t0 = clk::now();
    for (int i = 0; i < n; ++i) sink = sink + fn();
    const double ns = std::chrono::duration<double, std::nano>(clk::now() - t0).count() / n;
    std::printf("[TB] bench %s: %.1f ns per read\n", name, ns);
  };
  run("steady_clock::now", [] { return (uint64_t)clk::now().time_since_epoch().count(); });
  run("chrono timebase (r8)", [&] { return chrono_timebase(c); });
#ifdef __SWITCH__
  run("armGetSystemTick", [] { return armGetSystemTick(); });
  run("tick timebase", [&] { return tick_timebase(c); });
  run("svcGetSystemTick", [] { return svcGetSystemTick(); });
#endif
  std::fflush(stdout);
}

uint64_t CPUContext::wall_timebase_now() const {
  // 0 = not decided yet, 1 = chrono, 2 = tick
  static int src = 0;
  if (__builtin_expect(src == 0, 0)) {
    const char *e = NWII_ENV("NWII_TB_SRC");
#ifdef __SWITCH__
    src = (e && std::strcmp(e, "chrono") == 0) ? 1 : 2;
#else
    src = 1;
    (void)e;
#endif
    std::printf("[TB] host clock: %s\n", src == 2 ? "system tick (x135/64)" : "steady_clock (us)");
    if (NWII_ENV("NWII_TB_BENCH")) tb_bench(*this);
  }
#ifdef __SWITCH__
  if (src == 2) return tick_timebase(*this);
#endif
  return chrono_timebase(*this);
}


// The OS idle loop (SelectThread with an empty run queue, e.g. 0x801B4FF8 in
// SMNE01) calls this on every turn. With the real-time clock nothing in the
// guest can change until the next timed event, so sleep until then instead
// of spinning a host core at 100%.
// ---- Overload governor (cpu_context.h) ----------------------------------
// Runs every 10 ms of wall time from the timebase refresh. "Busy" is the wall
// time since the guest last idled (idle_wait: OS idle loop or a polling loop).
//   busy >= starve (default 1 s):  guest clock rate x3/4 every 50 ms, down
//                                  to 1/16 (0.5 s from 1 to the floor)
//   busy <  starve:                rate + 1/256 per step up to `ceiling`, then
//                                  + 1/4096 per step (see below)
// A host that keeps up idles every frame (busy < 17 ms; < 0.5 s while booting
// on a fast PC) and never leaves rate 1. A slow host that still idles now and
// then (Switch title screen at 7-8 fps: the game waits for the next VI after
// each frame) is left alone too: the full-rate audio there costs some fps but
// the game keeps running. Only a guest that cannot reach idle for a whole
// second (the AX interrupt livelock) is slowed down.
// NWII_GOV=0 disables it; NWII_GOV_STARVE_MS / NWII_GOV_MIN (1/n) tune it.
std::atomic<uint64_t> g_gov_max_busy_us{0};  // longest busy stretch (NWII_FPS)
std::atomic<uint64_t> g_gov_slip_us{0};      // total slip so far
std::atomic<uint32_t> g_gov_min_rate{CPUContext::kGovOne};  // since last report

extern std::atomic<uint64_t> g_interp_calls;  // interpreter.cpp

// ", busy max .. ms, clock x.. , slip .. ms, interp ../s" for the periodic
// NWII_FPS / Switch fps_log lines; resets the per-report maxima.
void GX_WGPIPE_Flush();  // gx.cpp
int GX_WgpStatsLine(char *buf, int size, double dt);  // gx.cpp
void poll_stats_report(char *buf, size_t n, double dt);  // ios.cpp (R9 guestloop)

void governor_report(char *buf, size_t n, double dt) {
  static uint64_t interp0 = 0;
  const uint64_t interp = g_interp_calls.load(std::memory_order_relaxed);
  const uint64_t busy_ms = g_gov_max_busy_us.exchange(0, std::memory_order_relaxed) / 1000;
  const uint32_t rate = g_gov_min_rate.exchange(CPUContext::kGovOne, std::memory_order_relaxed);
  std::snprintf(buf, n, ", busy max %llu ms, clock x%.2f, slip %llu ms, interp %.0f/s",
                (unsigned long long)busy_ms, rate / (double)CPUContext::kGovOne,
                (unsigned long long)(g_gov_slip_us.load(std::memory_order_relaxed) / 1000),
                dt > 0 ? (interp - interp0) / dt : 0.0);
  interp0 = interp;
  // R8: AX command lists (where they mix, cost, CPU-thread waits).
  const size_t used = std::strlen(buf);
  if (used + 1 < n)
    ax::stats_line(buf + used, (int)(n - used));
  // R8 threads: GX write-gather buffer (FIFO writes and hand-overs per second).
  const size_t used2 = std::strlen(buf);
  if (used2 + 1 < n)
    GX_WgpStatsLine(buf + used2, (int)(n - used2), dt);
  // R9 guestloop: interrupt poll counters (NWII_POLL_STATS).
  const size_t used3 = std::strlen(buf);
  if (used3 + 1 < n) poll_stats_report(buf + used3, n - used3, dt);
}

void CPUContext::governor_step(uint64_t wall) const {
  gov_step_wall = wall;
  if (this != g_ctx_ptr && g_ctx_ptr) return;  // r34s2: el contexto de sonido no gobierna el reloj
  static const bool off = [] {
    const char *e = NWII_ENV("NWII_GOV");
    return e && (e[0] == '0' || e[0] == 'n' || e[0] == 'o' || e[0] == 'f');
  }();
  static const uint64_t starve = [this] {
    const char *e = NWII_ENV("NWII_GOV_STARVE_MS");
    const uint64_t ms = e ? std::max<uint64_t>(std::strtoull(e, nullptr, 10), 20) : 1000;
    return tb_freq * ms / 1000;
  }();
  static const uint32_t floor = [] {
    const char *e = NWII_ENV("NWII_GOV_MIN");
    const uint64_t n = e ? std::max<uint64_t>(std::strtoull(e, nullptr, 10), 1) : 16;
    return (uint32_t)(kGovOne / std::min<uint64_t>(n, 1024));
  }();
  const uint64_t busy = wall - gov_idle_wall;
  const uint64_t busy_us = busy * 1000000 / tb_freq;
  if (busy_us > g_gov_max_busy_us.load(std::memory_order_relaxed))
    g_gov_max_busy_us.store(busy_us, std::memory_order_relaxed);
  if (off)
    return;
  const uint32_t before = gov_rate;
  // `ceiling`: 3/4 of the rate at which the guest last starved. Below it the
  // rate climbs fast, above it slowly, so a host that is too slow for real
  // time settles just under its limit instead of starving again every few
  // seconds. It is lifted after 20 s without starvation.
  static uint64_t last_cut = 0, last_starve = 0;
  static uint32_t ceiling = kGovOne;
  static bool in_episode = false;
  // R9 guestloop: longest busy stretch over the current and the previous 2 s
  // window (see the climb below).
  static uint64_t win_start = 0, win_max = 0, prev_win_max = UINT64_MAX;
  if (wall - win_start >= 2 * tb_freq) {
    prev_win_max = win_max;
    win_max = 0;
    win_start = wall;
  }
  win_max = std::max(win_max, busy);
  if (busy >= starve) {
    if (!in_episode) {
      in_episode = true;
      ceiling = std::max<uint32_t>(gov_rate - gov_rate / 4, floor);
    }
    last_starve = wall;
    // The guest needs a while to reach its next idle point even once the
    // interrupt load drops, so cut at most every 50 ms.
    if (wall - last_cut >= tb_freq / 20) {
      last_cut = wall;
      gov_rate = std::max<uint32_t>(gov_rate - gov_rate / 4, floor);
    }
  } else {
    in_episode = false;
    if (ceiling < kGovOne && wall - last_starve >= 20 * tb_freq)
      ceiling = kGovOne;
    // R9 guestloop: above the ceiling the slow climb (1/4096 per step, ~10 s
    // from x0.75 to x1) is only needed while the guest is still close to
    // starving. A guest that idled within the last starve/4 (250 ms: in
    // citron the World 1 map idles every 60-140 ms at x1) truly keeps up, so
    // it climbs at the fast rate to real time (r8: the map ran 10-13 s at
    // x0.78-0.90 after its loading episode, with audio and timers slowed).
    // NWII_GOV_SLOWCLIMB=1 restores the r8 rule.
    // "Within the last 250 ms" must hold for the last 2-4 s (windowed max
    // of `busy`), so a host that only just keeps up at the ceiling still
    // settles slowly instead of oscillating around its limit.
    static const bool slow_climb = NWII_ENV("NWII_GOV_SLOWCLIMB") != nullptr;
    const bool keeps_up = std::max(prev_win_max, win_max) < starve / 4;
    const bool fast = gov_rate < ceiling || (!slow_climb && keeps_up);
    if (gov_rate < kGovOne)
      gov_rate = std::min<uint32_t>(gov_rate + (fast ? kGovOne / 256 : kGovOne / 4096), kGovOne);
  }
  if (gov_rate < g_gov_min_rate.load(std::memory_order_relaxed))
    g_gov_min_rate.store(gov_rate, std::memory_order_relaxed);
  g_gov_slip_us.store(gov_slip * 1000000 / tb_freq, std::memory_order_relaxed);
  if ((before == kGovOne) != (gov_rate == kGovOne)) {
    static uint32_t transitions = 0;
    static uint64_t slip_at_start = 0, wall_at_start = 0;
    if (gov_rate < kGovOne) {
      slip_at_start = gov_slip;
      wall_at_start = wall;
    }
    if (transitions++ < 100 || transitions % 100 == 0) {
      if (gov_rate < kGovOne)
        std::cout << "[Gov] guest idle for none of the last " << busy_us / 1000
                  << " ms: slowing the guest clock (event #" << transitions << ")"
                  << std::endl;
      else
        std::cout << "[Gov] guest clock back to real time after "
                  << (wall - wall_at_start) * 1000 / tb_freq << " ms, slipped "
                  << (gov_slip - slip_at_start) * 1000 / tb_freq << " ms (total "
                  << gov_slip * 1000 / tb_freq << " ms)" << std::endl;
    }
  }
}

extern int64_t g_v_idle_ns;  // r37v (ios.cpp): NWII_V_LATE
extern bool g_v_vi_held;
void v_deliver_held(CPUContext &c, int why);
static inline int64_t v_now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct VtlIdle {  // r37v: NWII_V_TL (tramos ociosos) y NWII_V_LATE (hora de la ultima vuelta ociosa)
  bool on;
  static bool late_on() {
    static const bool v = [] { const char *e = std::getenv("NWII_V_LATE"); return e && e[0] && e[0] != '0'; }();
    return v;
  }
  explicit VtlIdle(CPUContext &c) : on(&c == g_ctx_ptr) {
    if (on) { ::vtl_idle_turn(false); if (late_on()) g_v_idle_ns = v_now_ns(); }
  }
  ~VtlIdle() { if (on) { ::vtl_idle_turn(true); if (late_on()) g_v_idle_ns = v_now_ns(); } }
};
void idle_wait(CPUContext &ctx) {
  VtlIdle vtl_idle_(ctx);
  // r37v NWII_V_LATE: el juego ya espera (bucle ocioso): lanzar ahora el retrazo retenido.
  if (__builtin_expect(g_v_vi_held, 0) && &ctx == g_ctx_ptr) {
    v_deliver_held(ctx, 1);
    return;
  }
  // R8 threads: hand the guest's gathered GX FIFO bytes to the host before
  // the guest idles (see the write-gather buffer in gx.cpp).
  GX_WGPIPE_Flush();
  if (!CPUContext::wall_clock_tb())
    return;
  ctx.note_idle();
  if (NWII_ENV("NWII_NO_IDLE_SLEEP"))
    return;
  // Anything that is about to raise an interrupt is counted in polls, not
  // time, or comes from the host thread: keep spinning for those.
  if ((hw::pi_intsr & hw::pi_intmr) || ctx.vblank_pending ||
      g_ipc_interrupt_delay > 0 || hw::g_di_interrupt_delay > 0 ||
      ctx.pending_cb_count.load(std::memory_order_relaxed) != 0)
    return;
  // r37v (frente V), NWII_V_WAKE=1: si desde la vuelta anterior del bucle ocioso se le entrego al invitado
  // una interrupcion / DEC / callback (que puede haber despertado un hilo: el retrazo VI despierta al hilo
  // principal), no dormir: el bucle ocioso vuelve a mirar su cola de hilos listos en la vuelta siguiente.
  // Sin esto el hilo despertado esperaba lo que durase el sueno (hasta 2 ms) antes de correr: medido en PC,
  // retrazo -> copia XFB 2,07 ms (con NWII_NO_IDLE_SLEEP 0,02 ms), que se comia el presupuesto del fotograma.
  {
    static const bool v_wake = [] {
      const char *e = std::getenv("NWII_V_WAKE");
      return e && e[0] && e[0] != '0';
    }();
    static uint32_t seen = 0;
    if (v_wake && &ctx == g_ctx_ptr && g_v_dispatch_seq.load(std::memory_order_relaxed) != seen) {
      seen = g_v_dispatch_seq.load(std::memory_order_relaxed);
      return;
    }
  }
  uint64_t due = EventScheduler::get().next_due();
  if (ctx.dec_irq_pending && ctx.dec_value != 0xFFFFFFFF)
    due = std::min<uint64_t>(due, ctx.dec_written_tb + ctx.dec_value);
  // `due` is guest time: compare with the governed clock, and while the guest
  // clock runs slow a guest tick lasts kGovOne / gov_rate wall ticks.
  uint64_t now = ctx.guest_timebase_from_wall(ctx.wall_timebase_now());
  if (due <= now)
    return;
  // Clamp before scaling: with no event queued `due` is UINT64_MAX and the
  // product would overflow.
  uint64_t ticks = std::min<uint64_t>(due - now, ctx.tb_freq);
  if (ctx.gov_rate < CPUContext::kGovOne)
    ticks = std::min<uint64_t>(ticks * CPUContext::kGovOne / ctx.gov_rate, ctx.tb_freq);
  uint64_t us = ticks * 1000000 / ctx.tb_freq;
  // Host sleeps overshoot by ~60 us; wake a little early and spin the rest.
  // Cap the sleep so work the host thread hands over is picked up quickly.
  if (us < 200)
    return;
  const auto t0 = std::chrono::steady_clock::now();
  if (s2::g_enabled && (ctx.msr & 0x8000))
    s2::game_idle_sleep(std::min<uint64_t>(us - 100, 2000));  // r34s2: interrumpible por la CPU de sonido
  else
    std::this_thread::sleep_for(std::chrono::microseconds(std::min<uint64_t>(us - 100, 2000)));
  g_idle_sleeps.fetch_add(1, std::memory_order_relaxed);
  g_idle_sleep_us.fetch_add(
      (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0).count(),
      std::memory_order_relaxed);
  ctx.tb_invalidate();
}

}  // namespace nwii::runtime
