// R9 threads (test-only, linked into the Switch *_input_test.nro with
// -Wl,--wrap=pthread_create): which threads the NRO has and on which cores.
//
// Every pthread_create (SDL audio, Mesa util_queue, our CPU and AX threads...)
// goes through __wrap_pthread_create: the new thread logs
//   [THR] #k start=<offset in the NRO> core=<current core> pref=<preferred> mask=<affinity> prio=<p>
// when it starts, and a sampler thread (low priority, wakes every 10 s) logs the
// same for every known thread, plus the main thread:
//   [THR] t=<s> main pref=.. mask=.. | #1 pref=.. mask=.. | ...
// so a thread that changes its own mask later (the CPU and AX threads do) shows
// its final placement. Offsets map to symbols with
//   aarch64-none-elf-nm -C nx/build/fast_input/nsmbw.elf  (start = offset + 0, the
//   NRO is loaded at its link address 0 + base; we print start - &__start__).
// On by default in the test NRO; debug.toml NWII_NX_THREADDIAG = "0" turns the
// sampler off (the start lines are always written, once per thread).
#include <switch.h>

#include <pthread.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
int __real_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
extern char __start__;  // libnx crt0: start of the loaded NRO image
}

namespace {

constexpr int kMax = 32;
struct Rec {
  std::atomic<Handle> h{0};
  uintptr_t start = 0;
};
Rec g_recs[kMax];
std::atomic<int> g_n{0};
Handle g_main = 0;

struct Tramp {
  void *(*fn)(void *);
  void *arg;
  int idx;
};

void describe(Handle h, char *buf, size_t n) {
  s32 pref = -9;
  u64 mask = 0;
  s32 prio = -1;
  if (R_FAILED(svcGetThreadCoreMask(&pref, &mask, h))) pref = -9;
  if (R_FAILED(svcGetThreadPriority(&prio, h))) prio = -1;
  std::snprintf(buf, n, "pref=%d mask=0x%llx prio=0x%x", (int)pref, (unsigned long long)mask,
                (unsigned)prio);
}

void *tramp_entry(void *p) {
  Tramp t = *(Tramp *)p;
  std::free(p);
  if (t.idx >= 0) g_recs[t.idx].h.store(threadGetCurHandle());
  char d[96];
  describe(threadGetCurHandle(), d, sizeof d);
  std::printf("[THR] #%d start=+0x%lx core=%d %s\n", t.idx,
              (unsigned long)(t.idx >= 0 ? g_recs[t.idx].start : 0),
              (int)svcGetCurrentProcessorNumber(), d);
  return t.fn(t.arg);
}

void *sampler(void *) {
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 1);  // core 0, lowest priority
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x3F);
  for (int s = 10;; s += 10) {
    svcSleepThread(10'000'000'000ll);
    char line[1024];
    size_t u = 0;
    char d[96];
    describe(g_main, d, sizeof d);
    u += std::snprintf(line + u, sizeof line - u, "[THR] t=%d main %s", s, d);
    const int n = g_n.load();
    for (int i = 0; i < n && u < sizeof line; ++i) {
      const Handle h = g_recs[i].h.load();
      if (!h) continue;
      describe(h, d, sizeof d);
      u += std::snprintf(line + u, sizeof line - u, " | #%d %s", i, d);
    }
    std::printf("%s\n", line);
  }
  return nullptr;
}

void start_sampler() {
  static bool started = false;
  if (started) return;
  started = true;
  g_main = threadGetCurHandle();
  const char *e = std::getenv("NWII_NX_THREADDIAG");
  if (e && e[0] == '0') return;
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, 64 * 1024);
  pthread_t th;
  if (__real_pthread_create(&th, &a, sampler, nullptr) == 0) pthread_detach(th);
  std::printf("[THR] main thread core=%d (thread diag on, NWII_NX_THREADDIAG)\n",
              (int)svcGetCurrentProcessorNumber());
}

}  // namespace

extern "C" int __wrap_pthread_create(pthread_t *th, const pthread_attr_t *attr,
                                     void *(*fn)(void *), void *arg) {
  // The first pthread_create comes from the main thread (SDL init / CPU thread).
  start_sampler();
  const int idx = g_n.fetch_add(1);
  Tramp *t = (Tramp *)std::malloc(sizeof(Tramp));
  if (!t) return __real_pthread_create(th, attr, fn, arg);
  t->fn = fn;
  t->arg = arg;
  t->idx = idx < kMax ? idx : -1;
  if (t->idx >= 0) g_recs[t->idx].start = (uintptr_t)fn - (uintptr_t)&__start__;
  return __real_pthread_create(th, attr, tramp_entry, t);
}
