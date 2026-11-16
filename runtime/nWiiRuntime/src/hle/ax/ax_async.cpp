// R8 (threads): where the AX command list runs.
//
// On the Wii the DSP mixes beside the CPU: the AX library mails it the list
// address and carries on; the DSP answers DSP_SYNC when the frame is mixed
// (hw_dsp.cpp posts it 0.1 ms of guest time after the list mail). r7b mixed
// the whole list on the CPU thread inside the mailbox write, ~1000 times a
// second counting the other AX interrupts; on the Switch the CPU thread is the
// bottleneck and cores 0/2 have room.
//
// NWII_AX_THREAD
//   2 (default)  the list runs on the AX thread (Switch: core 2) and DSP_SYNC
//                goes out when the mix is done, at 0.1 ms of guest time or
//                later (at the latest with DSP_YIELD, 1 ms): like the real
//                DSP, which answers once it has mixed. The CPU thread does not
//                wait for the mix (measured on citron: ~150 us per list).
//   1            the list runs on the AX thread, but the DSP_SYNC event waits
//                for it at exactly 0.1 ms, the r7b mail timing.
//   0            inline in the mailbox write, as r7b.
// In 1 and 2, if the AX thread has not even started the list when the CPU
// thread needs it, the CPU thread mixes it itself (never worse than inline).
// Rule that makes 1 and 2 exact: between the list mail and DSP_SYNC/YIELD the guest
// does not touch what the list reads or writes (voice blocks, aux and output
// buffers, sample data). The AX library is written for a DSP that is busy for
// a while, so it does not; NWII_AX_HAZARD=1 (PC, x86-64 Linux) checks it with
// page protection, NWII_AX_VERIFY=1 checks the optimized mixer against the
// r7b one (ax_hle_ref.cpp) on every list.
#include "ax_hle.h"

#include "runtime/cpu_context.h"
#include "runtime/event_scheduler.h"
#include "runtime/nwii_env.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef __SWITCH__
#include <switch.h>
#include <pthread.h>
#include "../../platform/switch/switch_platform.h"
#endif

#if defined(__linux__) && defined(__x86_64__)
#define NWII_AX_HAZARD_SUPPORTED 1
#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace nwii::runtime {
extern MMU *g_mmu;
extern CPUContext *g_ctx_ptr;
}  // namespace nwii::runtime

namespace nwii::runtime::ax {
namespace {

using clk = std::chrono::steady_clock;

uint64_t now_ns() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
             clk::now().time_since_epoch())
      .count();
}

bool env_on(const char *v) { return v && v[0] && v[0] != '0'; }

// Switches are read on first use, not at static-init time: on the Switch the
// debug.toml NWII_* values are put in the environment by early_init().
struct Opts {
  bool verify, ref_mixer, hazard;
  int mode;
};
const Opts &opts() {
  static const Opts o = [] {
    Opts r{};
    r.verify = env_on(std::getenv("NWII_AX_VERIFY"));
    // NWII_AX_MIXER=ref: mix with the r7b scalar code (timing comparisons only).
    const char *m = std::getenv("NWII_AX_MIXER");
    r.ref_mixer = m && std::strcmp(m, "ref") == 0;
#ifdef NWII_AX_HAZARD_SUPPORTED
    r.hazard = env_on(std::getenv("NWII_AX_HAZARD"));
#endif
    const char *e = std::getenv("NWII_AX_THREAD");
    r.mode = (!e || !e[0]) ? 2 : std::clamp(std::atoi(e), 0, 2);
    if (r.hazard || r.verify) r.mode = 0;  // both mix inline, at the mail write
    return r;
  }();
  return o;
}

// ---- stats (for the [FPS] / fps_log line) ----
std::atomic<uint64_t> s_lists{0}, s_mix_ns{0}, s_mix_max_ns{0}, s_wait_ns{0}, s_waits{0};

// ---- NWII_AX_VERIFY: optimized mixer vs the r7b reference ----
struct Verify {
  std::unordered_map<const uint8_t *, uint8_t> overlay;
  RangeLog wlog;
  std::vector<uint8_t> sa, sb;
  uint64_t lists = 0, bad_lists = 0, bad_bytes = 0, extra_bytes = 0, bad_state = 0,
           bad_ret = 0;
};
Verify *g_v = nullptr;

bool run_verified(uint32_t addr, uint16_t size) {
  if (!g_v) g_v = new Verify;
  Verify &v = *g_v;
  v.overlay.clear();
  ref::g_overlay = &v.overlay;
  const bool pr = ref::run_command_list(addr, size);
  v.wlog.clear();
  g_write_log = &v.wlog;
  const bool pn = run_command_list(addr, size);
  g_write_log = nullptr;
  uint64_t bad = 0, extra = 0;
  for (const auto &[p, b] : v.overlay)
    if (*p != b) ++bad;
  for (const auto &[p, n] : v.wlog)
    for (uint32_t i = 0; i < n; ++i)
      if (!v.overlay.count(p + i)) ++extra;
  ref::state_bytes(v.sa);
  state_bytes(v.sb);
  const bool state_ok = v.sa == v.sb;
  ++v.lists;
  if (bad || extra || !state_ok || pr != pn) {
    ++v.bad_lists;
    if (v.bad_lists <= 10)
      std::printf("[AXV] MISMATCH list #%llu @%08x: %llu bytes differ, %llu extra bytes "
                  "written, state %s, produced ref=%d new=%d\n",
                  (unsigned long long)v.lists, addr, (unsigned long long)bad,
                  (unsigned long long)extra, state_ok ? "same" : "DIFFERS", pr, pn);
    // Keep comparing from the reference's state.
    if (!state_ok) std::printf("[AXV] (state differs: later lists may differ too)\n");
  }
  v.bad_bytes += bad;
  v.extra_bytes += extra;
  v.bad_state += !state_ok;
  v.bad_ret += pr != pn;
  if (v.lists % 3000 == 0)
    std::printf("[AXV] %llu lists checked, %llu differ (bytes %llu, extra %llu, state %llu, "
                "ret %llu), %zu bytes written by the last list\n",
                (unsigned long long)v.lists, (unsigned long long)v.bad_lists,
                (unsigned long long)v.bad_bytes, (unsigned long long)v.extra_bytes,
                (unsigned long long)v.bad_state, (unsigned long long)v.bad_ret,
                v.overlay.size());
  return pn;
}

// Mixes one list (already copied out of guest memory unless verifying).
bool run_one(uint32_t addr, uint16_t size, const uint16_t *list, uint32_t n) {
  const uint64_t t0 = now_ns();
  const bool produced = opts().verify       ? run_verified(addr, size)
                        : opts().ref_mixer ? ref::run_command_list(addr, size)
                                      : run_loaded_list(list, n);
  const uint64_t dt = now_ns() - t0;
  s_lists.fetch_add(1, std::memory_order_relaxed);
  s_mix_ns.fetch_add(dt, std::memory_order_relaxed);
  if (dt > s_mix_max_ns.load(std::memory_order_relaxed))
    s_mix_max_ns.store(dt, std::memory_order_relaxed);
  return produced;
}

bool run_inline(uint32_t addr, uint16_t size) {
  uint16_t list[kMaxListWords];
  const uint32_t n = load_command_list(addr, size, list);
  return run_one(addr, size, list, n);
}

// ---- the AX thread ----
// One list at a time in one slot. The CPU thread posts it and wakes the AX
// thread; whoever claims the slot first mixes it. If the AX thread has not
// started by the time the CPU thread needs the result (the host is busy and
// did not schedule it), the CPU thread mixes the list itself: never slower
// than inline, apart from the wake-up call.
enum Slot : uint32_t { kEmpty, kPosted, kRunning, kStolen, kDone };
struct Worker {
  std::mutex m;
  std::condition_variable cv_job, cv_done;
  uint32_t addr = 0;
  uint16_t size = 0;
  uint16_t list[kMaxListWords];  // the list, copied at the mail write
  uint32_t n = 0;
  uint64_t seq_req = 0;  // written by the CPU thread under m
  std::atomic<uint32_t> slot{kEmpty};
  bool produced = false;  // valid once slot == kDone (release/acquire on slot)

  void loop() {
#ifdef __SWITCH__
    // Homebrew gets cores 0-2: the main (render) thread runs on the default
    // core 0, the CPU thread on core 1 (switch_platform.cpp); audio takes 2.
    u64 allowed = 0;
    svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    int core = -1;
    for (int c : {2, 0})
      if ((allowed & (1ull << c)) &&
          R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, c, 1u << c))) {
        core = c;
        break;
      }
    // R14 R5: libnx starts pthreads at priority 0x3B (the lowest), below the
    // render worker (0x2D) that shares core 2: every list the worker
    // preempted made the CPU thread wait in finish() (console, castle
    // cutscene: ~70 ms of waits per second, mixes up to 42 ms). One step above
    // the worker, as the worker's own comment assumed. NWII_AX_PRIO overrides.
    const char *pe = std::getenv("NWII_AX_PRIO");
    const int prio = pe ? (int)std::strtol(pe, nullptr, 0) : 0x2C;
    const Result prc = svcSetThreadPriority(CUR_THREAD_HANDLE, prio);
    std::printf("[NX] AX thread on core %d, priority 0x%X%s (allowed mask 0x%llx)\n", core, prio,
                R_SUCCEEDED(prc) ? "" : " (failed)", (unsigned long long)allowed);
    nx::note_thread(nx::kThreadAx);
#endif
    uint64_t seen = 0;
    for (;;) {
      uint32_t a, n_words;
      uint16_t s;
      {
        std::unique_lock<std::mutex> lk(m);
        cv_job.wait(lk, [&] { return seq_req != seen; });
        seen = seq_req;
        // Claim under m: a new list is only posted under m, so addr/size
        // belong to the list in the slot.
        uint32_t expect = kPosted;
        if (!slot.compare_exchange_strong(expect, kRunning, std::memory_order_acquire))
          continue;  // the CPU thread took it
        a = addr;
        s = size;
        n_words = n;
      }
      produced = run_one(a, s, list, n_words);
      {
        std::lock_guard<std::mutex> lk(m);
        slot.store(kDone, std::memory_order_release);
      }
      cv_done.notify_one();
    }
  }
};
Worker *g_w = nullptr;  // never destroyed: the thread runs until exit
std::atomic<uint64_t> s_steals{0};

Worker &worker() {
  if (!g_w) {
    g_w = new Worker;
#ifdef __SWITCH__
    // Explicit stack (std::thread cannot size it on libnx).
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    pthread_t th;
    pthread_create(
        &th, &attr,
        [](void *) -> void * {
          g_w->loop();
          return nullptr;
        },
        nullptr);
    pthread_detach(th);
#else
    std::thread([] { g_w->loop(); }).detach();
#endif
    std::printf("[AX] command lists run on their own thread (NWII_AX_THREAD=%d%s)\n", opts().mode,
                opts().mode == 2 ? ": DSP_SYNC slides" : "");
  }
  return *g_w;
}

bool s_inline_produced = false;

#ifdef NWII_AX_HAZARD_SUPPORTED
// ---- NWII_AX_HAZARD: does the guest touch the DSP's data inside the window?
// The list is mixed inline with read/write logs. Pages holding bytes the list
// wrote become PROT_NONE, pages holding only bytes it read PROT_READ, until
// NWII_AX_HAZARD_US (default 1000 = up to DSP_YIELD) of guest time later. A
// fault on a byte the list wrote (read or write) or a write to a byte it read
// is a hazard: with the AX thread that access could see / clobber a half-done
// mix. Faults on other bytes of those pages are let through by single-stepping
// the instruction with the page unprotected.
namespace hz {
constexpr long kPage = 4096;
struct Rng {
  uintptr_t lo, hi;  // [lo, hi)
};
std::vector<Rng> g_out, g_in;  // merged, sorted; only read inside the window
struct Pg {
  uintptr_t page;
  int prot;
};
std::vector<Pg> g_pages;
volatile sig_atomic_t g_armed = 0;
uintptr_t g_m1_lo, g_m1_hi, g_m2_lo, g_m2_hi;
RangeLog g_rlog, g_wlog;

constexpr int kMaxHits = 4096;
struct Hit {
  uintptr_t addr, rip;
  uint32_t guest_pc, guest_lr;
  uint64_t tb;
  uint8_t write, out, cpu_thread;
};
uint32_t g_list_addr = 0, g_list_bytes = 0;
uint64_t g_submit_tb = 0;
Hit g_hits[kMaxHits];
std::atomic<int> g_nhits{0};
std::atomic<uint64_t> g_faults{0}, g_hit_total{0};
uint64_t g_windows = 0, g_hit_windows = 0;
std::thread::id g_cpu_tid;
thread_local uintptr_t t_step_pages[4];
thread_local int t_nstep = 0;

bool in_ranges(const std::vector<Rng> &v, uintptr_t a) {
  auto it = std::upper_bound(v.begin(), v.end(), a,
                             [](uintptr_t x, const Rng &r) { return x < r.lo; });
  return it != v.begin() && a < (it - 1)->hi;
}

int page_prot(uintptr_t page) {
  auto it = std::lower_bound(g_pages.begin(), g_pages.end(), page,
                             [](const Pg &p, uintptr_t x) { return p.page < x; });
  return it != g_pages.end() && it->page == page ? it->prot : -1;
}

uint32_t guest_addr(uintptr_t a) {
  if (a >= g_m1_lo && a < g_m1_hi) return (uint32_t)(a - g_m1_lo) | 0x80000000u;
  if (a >= g_m2_lo && a < g_m2_hi) return (uint32_t)(a - g_m2_lo) | 0x90000000u;
  return 0;
}

void on_segv(int, siginfo_t *si, void *uc_) {
  ucontext_t *uc = (ucontext_t *)uc_;
  const uintptr_t a = (uintptr_t)si->si_addr;
  const uintptr_t page = a & ~(uintptr_t)(kPage - 1);
  const bool ours = (a >= g_m1_lo - kPage && a < g_m1_hi + kPage) ||
                    (a >= g_m2_lo - kPage && a < g_m2_hi + kPage);
  if (!ours) {
    signal(SIGSEGV, SIG_DFL);  // a real crash: let it happen
    return;
  }
  g_faults.fetch_add(1, std::memory_order_relaxed);
  const int prot = g_armed ? page_prot(page) : -1;
  if (prot >= 0) {
    const bool write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
    const bool out = in_ranges(g_out, a);
    if (out || (write && in_ranges(g_in, a))) {
      g_hit_total.fetch_add(1, std::memory_order_relaxed);
      const int i = g_nhits.fetch_add(1, std::memory_order_relaxed);
      if (i < kMaxHits) {
        CPUContext *c = g_ctx_ptr;
        g_hits[i] = {a, (uintptr_t)uc->uc_mcontext.gregs[REG_RIP], c ? c->pc : 0,
                     c ? c->lr : 0, c ? c->tb_cached : 0, (uint8_t)write, (uint8_t)out,
                     (uint8_t)(std::this_thread::get_id() == g_cpu_tid)};
      }
    }
  }
  mprotect((void *)page, kPage, PROT_READ | PROT_WRITE);
  if (prot >= 0 && t_nstep < 4) {
    t_step_pages[t_nstep++] = page;
    uc->uc_mcontext.gregs[REG_EFL] |= 0x100;  // trap after this instruction
  }
}

void on_trap(int, siginfo_t *, void *uc_) {
  ucontext_t *uc = (ucontext_t *)uc_;
  uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
  for (int i = 0; i < t_nstep; ++i) {
    const int prot = g_armed ? page_prot(t_step_pages[i]) : -1;
    if (prot >= 0) mprotect((void *)t_step_pages[i], kPage, prot);
  }
  t_nstep = 0;
}

void install() {
  static bool done = false;
  if (done) return;
  done = true;
  g_cpu_tid = std::this_thread::get_id();
  g_m1_lo = (uintptr_t)g_mmu->mem1.data();
  g_m1_hi = g_m1_lo + g_mmu->mem1.size();
  g_m2_lo = (uintptr_t)g_mmu->mem2.data();
  g_m2_hi = g_m2_lo + g_mmu->mem2.size();
  struct sigaction sa {};
  sa.sa_sigaction = on_segv;
  sa.sa_flags = SA_SIGINFO | SA_NODEFER;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, nullptr);
  sa.sa_sigaction = on_trap;
  sigaction(SIGTRAP, &sa, nullptr);
  std::printf("[AXH] hazard checker on: guarding the DSP's data for %s us of guest time "
              "after each list\n",
              NWII_ENV("NWII_AX_HAZARD_US") ? NWII_ENV("NWII_AX_HAZARD_US") : "1000");
}

std::vector<Rng> merge(RangeLog &log) {
  std::vector<Rng> v;
  v.reserve(log.size());
  for (auto &[p, n] : log) v.push_back({(uintptr_t)p, (uintptr_t)p + n});
  std::sort(v.begin(), v.end(), [](const Rng &a, const Rng &b) { return a.lo < b.lo; });
  std::vector<Rng> m;
  for (const Rng &r : v) {
    if (!m.empty() && r.lo <= m.back().hi) m.back().hi = std::max(m.back().hi, r.hi);
    else m.push_back(r);
  }
  return m;
}

void disarm();

void arm() {
  g_out = merge(g_wlog);
  g_in = merge(g_rlog);
  g_pages.clear();
  auto add = [](const std::vector<Rng> &v, int prot) {
    for (const Rng &r : v)
      for (uintptr_t pg = r.lo & ~(uintptr_t)(kPage - 1); pg < r.hi; pg += kPage)
        g_pages.push_back({pg, prot});
  };
  add(g_out, PROT_NONE);
  add(g_in, PROT_READ);
  std::sort(g_pages.begin(), g_pages.end(), [](const Pg &a, const Pg &b) {
    return a.page != b.page ? a.page < b.page : a.prot < b.prot;  // PROT_NONE first
  });
  g_pages.erase(std::unique(g_pages.begin(), g_pages.end(),
                            [](const Pg &a, const Pg &b) { return a.page == b.page; }),
                g_pages.end());
  g_nhits.store(0);
  g_armed = 1;
  for (const Pg &p : g_pages) mprotect((void *)p.page, kPage, p.prot);
  ++g_windows;
}

void disarm() {
  if (!g_armed) return;
  g_armed = 0;
  for (const Pg &p : g_pages) mprotect((void *)p.page, kPage, PROT_READ | PROT_WRITE);
  const int n = std::min(g_nhits.load(), kMaxHits);
  if (n) {
    ++g_hit_windows;
    if (g_hit_windows <= 20)
      for (int i = 0; i < n && i < 6; ++i) {
        const Hit &h = g_hits[i];
        std::printf("[AXH] HAZARD window %llu (list %08x+%x): %s %s byte %08x (%s) at +%.1f us "
                    "rip=%p pc=%08x lr=%08x\n",
                    (unsigned long long)g_windows, g_list_addr, g_list_bytes,
                    h.cpu_thread ? "cpu thread" : "other thread", h.write ? "wrote" : "read",
                    guest_addr(h.addr), h.out ? "the list writes it" : "the list reads it",
                    (double)(int64_t)(h.tb - g_submit_tb) * 1e6 / g_ctx_ptr->tb_freq,
                    (void *)h.rip, h.guest_pc, h.guest_lr);
      }
  }
  if (g_windows % 3000 == 0)
    std::printf("[AXH] %llu windows, %llu with hazards (%llu hazard accesses), %llu page "
                "faults, last window %zu pages\n",
                (unsigned long long)g_windows, (unsigned long long)g_hit_windows,
                (unsigned long long)g_hit_total.load(), (unsigned long long)g_faults.load(),
                g_pages.size());
}

void submit_guarded(uint32_t addr, uint16_t size) {
  install();
  disarm();  // a new list before the old window closed
  g_list_addr = addr;
  g_list_bytes = size * 2u;
  g_submit_tb = g_ctx_ptr->read_timebase();
  g_rlog.clear();
  g_wlog.clear();
  g_read_log = &g_rlog;
  g_write_log = &g_wlog;
  s_inline_produced = run_inline(addr, size);
  if (uint8_t *p = g_mmu->get_ptr(addr))  // the list words it used
    g_rlog.push_back({p, last_list_words_used() * 2u});
  g_read_log = nullptr;
  g_write_log = nullptr;
  arm();
  static const uint64_t us = [] {
    const char *e = NWII_ENV("NWII_AX_HAZARD_US");
    return e ? (uint64_t)std::atoll(e) : 1000ull;
  }();
  EventScheduler::get().schedule_after(us * g_ctx_ptr->tb_freq / 1000000,
                                       [](CPUContext &, uint64_t) { disarm(); });
}
}  // namespace hz
#endif

}  // namespace

bool async_enabled() { return opts().mode != 0; }
bool sync_slides() { return opts().mode == 2; }

// Time of the first list: the first stats report counts from here.
static std::atomic<uint64_t> s_first_ns{0};

void submit(uint32_t addr, uint16_t size_words) {
  if (!s_first_ns.load(std::memory_order_relaxed))
    s_first_ns.store(now_ns(), std::memory_order_relaxed);
#ifdef NWII_AX_HAZARD_SUPPORTED
  if (opts().hazard) {
    hz::submit_guarded(addr, size_words);
    return;
  }
#endif
  if (opts().mode == 0) {
    s_inline_produced = run_inline(addr, size_words);
    return;
  }
  Worker &w = worker();
  finish();  // one list at a time (normally finished long ago)
  {
    std::lock_guard<std::mutex> lk(w.m);
    w.addr = addr;
    w.size = size_words;
    w.n = load_command_list(addr, size_words, w.list);
    w.slot.store(kPosted, std::memory_order_release);
    ++w.seq_req;
  }
  w.cv_job.notify_one();
}

bool mix_inline(uint32_t addr, uint16_t size_words) {
  if (!s_first_ns.load(std::memory_order_relaxed))
    s_first_ns.store(now_ns(), std::memory_order_relaxed);
  finish();  // a list still on the AX thread (handed over before the sound CPU took over)
  return run_inline(addr, size_words);
}

bool done() {
  if (opts().mode == 0 || !g_w) return true;
  const uint32_t s = g_w->slot.load(std::memory_order_acquire);
  return s == kDone || s == kEmpty;
}

bool finish() {
  if (opts().mode == 0 || !g_w) return s_inline_produced;
  Worker &w = *g_w;
  uint32_t s = w.slot.load(std::memory_order_acquire);
  if (s == kEmpty || s == kDone) return w.produced;
  if (s == kPosted && w.slot.compare_exchange_strong(s, kStolen, std::memory_order_acquire)) {
    w.produced = run_one(w.addr, w.size, w.list, w.n);  // written by this thread
    w.slot.store(kDone, std::memory_order_release);
    s_steals.fetch_add(1, std::memory_order_relaxed);
    return w.produced;
  }
  // The AX thread is mixing it: a mix is ~10-100 us; spin briefly, then sleep.
  const uint64_t t0 = now_ns();
  for (int i = 0; i < 200 && w.slot.load(std::memory_order_acquire) != kDone; ++i)
    std::this_thread::yield();
  if (w.slot.load(std::memory_order_acquire) != kDone) {
    std::unique_lock<std::mutex> lk(w.m);
    w.cv_done.wait(lk, [&] { return w.slot.load(std::memory_order_acquire) == kDone; });
  }
  s_wait_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
  s_waits.fetch_add(1, std::memory_order_relaxed);
  return w.produced;
}

void stats_line(char *buf, int size) {
  static uint64_t l0 = 0, m0 = 0, w0 = 0, n0 = 0, st0 = 0;
  static uint64_t t0 = 0;
  const uint64_t t = now_ns();
  if (!t0) t0 = s_first_ns.load(std::memory_order_relaxed);  // first report: since the first list
  const double dt = t0 && t > t0 ? (t - t0) / 1e9 : 0.0;
  const uint64_t l = s_lists.load(), m = s_mix_ns.load(), w = s_wait_ns.load(),
                 n = s_waits.load();
  const uint64_t mx = s_mix_max_ns.exchange(0);
  const uint64_t st = s_steals.load();
  const uint64_t dl = l - l0;
  std::snprintf(buf, size,
                ", ax %s %.0f lists/s mix %.1f us (max %.0f) wait %.0f us/s (%.0f/s) "
                "inline %.0f/s",
                opts().mode == 0 ? "inline" : "thread", dt > 0 ? dl / dt : 0.0,
                dl ? (m - m0) / 1e3 / dl : 0.0, mx / 1e3, dt > 0 ? (w - w0) / 1e3 / dt : 0.0,
                dt > 0 ? (n - n0) / dt : 0.0, dt > 0 ? (st - st0) / dt : 0.0);
  st0 = st;
  l0 = l;
  m0 = m;
  w0 = w;
  n0 = n;
  t0 = t;
}

}  // namespace nwii::runtime::ax
