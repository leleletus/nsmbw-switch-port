// r31t (frente T): perfil del hilo del juego con la pila invitada (NWII_TPROF=1).
//
// Cada muestra (1 ms) guarda: tiempo, pc/lr del anfitrion (relativos a la
// imagen), numero de fotograma del juego (copias XFB), escena de NWII_FORCE_DEMO,
// hilo invitado actual (0x800000E4) y dos pilas invitadas:
//   pila 1: la del contexto en marcha: [ctx.pc, ctx.lr, [sp+4], y luego, siguiendo
//           la cadena de marcos PPC (sp' = [sp]), cada direccion de retorno [sp'+4]].
//           El codigo generado guarda r1 en un registro local y solo lo baja a
//           ctx.gpr[1] antes de llamar: sp puede ser el marco de la funcion actual o
//           el de quien la llamo; por eso [sp+4] es solo un indicio (la funcion
//           exacta sale del pc del anfitrion) y el resto de la cadena es fiable.
//   pila 2: si el hilo actual no es el principal del juego (el que mas veces sale
//           como actual; no es el hilo por defecto del SO), la pila guardada del
//           principal [srr0, lr, cadena desde su r1 guardado]: dice donde esta
//           esperando (VIWaitForRetrace, GXDrawDone, lectura de disco...).
// Pilas sin repetir en una tabla (id = orden de aparicion desde 1). Archivos, que
// solo crecen (flush cada pocos segundos):
//   logs/tprof_info.txt     texto: base de la imagen, build, formato
//   logs/tprof_stacks.bin   por pila nueva: u32 n; u32 dir[n]
//   logs/tprof_samples.bin  por muestra: struct Rec (32 bytes)
// tools: r9/r31t/tools/tprof.py
#include "tprof.h"

#include "runtime/cpu_context.h"
#include "runtime/nwii_env.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef __SWITCH__
#include <csignal>
#include <ctime>
#include <sys/syscall.h>
#include <thread>
#include <ucontext.h>
#include <unistd.h>
extern char __executable_start;
#endif

namespace nwii::runtime {
extern std::atomic<uint32_t> g_guest_xfb;  // gx.cpp: fotogramas del juego (copias XFB)
std::atomic<int> g_tprof_scene{-1};        // force_demo.cpp la pone (indice de kDemos)
extern CPUContext *g_ctx_ptr;

namespace tprof {
namespace {

struct Rec {
  uint32_t t_us;     // desde init
  uint32_t pc, lr;   // anfitrion, relativos a la base de la imagen (0xFFFFFFFF: fuera)
  uint32_t stack;    // id de la pila 1 (0: tabla llena)
  uint32_t stack2;   // id de la pila 2 (0: no hay)
  uint32_t cur_thr;  // OSThread* actual (0 = bucle ocioso del SO)
  uint32_t hstack;   // id de la pila del anfitrion (retornos rebuscados; 0: no hay)
  uint16_t frame;    // g_guest_xfb
  int8_t scene;      // g_tprof_scene
  uint8_t flags;     // bit0: cur_thr == hilo principal
};
static_assert(sizeof(Rec) == 32, "Rec");

struct HRec {
  uint32_t t_us, pc, lr, hstack;
  uint16_t frame;
  int8_t scene;
  uint8_t slot;
};
static_assert(sizeof(HRec) == 20, "HRec");
constexpr uint32_t kMaxRec = 1u << 21;      // 2 M muestras (35 min a 1 kHz), 64 MB
constexpr uint32_t kArena = 12u << 20;      // 12 M u32 (48 MB) de pilas
constexpr uint32_t kSlots = 1u << 20;       // tabla hash: id por ranura
constexpr int kMaxDepth = 56;

Rec *g_rec = nullptr;
HRec *g_hrec = nullptr;
std::atomic<uint32_t> g_nhrec{0};
uint32_t g_hrec_written = 0;
uint32_t *g_arena = nullptr;  // [n, dir...] seguidos
uint32_t *g_slot_off = nullptr, *g_slot_id = nullptr;
std::atomic<uint32_t> g_nrec{0}, g_arena_used{0};
uint32_t g_nstacks = 0, g_rec_written = 0, g_arena_written = 0;
uint64_t g_overflow = 0;
uint64_t g_base = 0;
std::string g_dir;
bool g_on = false;

inline bool rd32(const CPUContext &c, uint32_t a, uint32_t &v) {
  const uint32_t p = a & 0x3FFFFFFF;
  const uint8_t *q;
  if ((a & 3) || (a >> 28) != 8 && (a >> 28) != 9) return false;
  if (p < MMU::kMem1Size - 4) q = c.mmu.mem1.data() + p;
  else if (p - 0x10000000u < MMU::kMem2Size - 4) q = c.mmu.mem2.data() + (p - 0x10000000u);
  else return false;
  uint32_t x;
  std::memcpy(&x, q, 4);
  v = __builtin_bswap32(x);
  return true;
}

uint32_t intern(const uint32_t *e, int n) {
  if (n <= 0) return 0;
  uint64_t h = 0x9E3779B97F4A7C15ull ^ (uint64_t)n;
  for (int i = 0; i < n; ++i) h = (h ^ e[i]) * 0xC2B2AE3D27D4EB4Full, h ^= h >> 29;
  for (uint32_t k = (uint32_t)(h >> 20);; ++k) {
    const uint32_t s = k & (kSlots - 1);
    if (g_slot_id[s] == 0) {
      const uint32_t used = g_arena_used.load(std::memory_order_relaxed);
      if (g_nstacks >= kSlots / 4 * 3 || used + n + 1 > kArena) {
        ++g_overflow;
        return 0;
      }
      g_arena[used] = (uint32_t)n;
      std::memcpy(g_arena + used + 1, e, (size_t)n * 4);
      g_slot_off[s] = used;
      g_slot_id[s] = ++g_nstacks;
      g_arena_used.store(used + n + 1, std::memory_order_release);
      return g_nstacks;
    }
    const uint32_t *a = g_arena + g_slot_off[s];
    if (a[0] == (uint32_t)n && std::memcmp(a + 1, e, (size_t)n * 4) == 0) return g_slot_id[s];
  }
}

// Cadena de marcos desde sp: anade cada [sp'+4].
int walk(const CPUContext &c, uint32_t sp, uint32_t *e, int n) {
  for (int k = 0; k < kMaxDepth && n < kMaxDepth + 4; ++k) {
    uint32_t nsp, ra;
    if (!rd32(c, sp, nsp) || nsp <= sp || nsp - sp > (4u << 20)) break;
    if (!rd32(c, nsp + 4, ra)) break;
    e[n++] = ra;
    sp = nsp;
  }
  return n;
}

}  // namespace

bool init(const char *log_dir, uint64_t image_base, const char *build) {
  const char *v = std::getenv("NWII_TPROF");
  if (!v || !*v || !std::strcmp(v, "0")) return false;
  if (g_on) return true;
  g_rec = (Rec *)std::calloc(kMaxRec, sizeof(Rec));
  g_hrec = (HRec *)std::calloc(kMaxRec, sizeof(HRec));
  g_arena = (uint32_t *)std::calloc(kArena, 4);
  g_slot_off = (uint32_t *)std::calloc(kSlots, 4);
  g_slot_id = (uint32_t *)std::calloc(kSlots, 4);
  if (!g_rec || !g_hrec || !g_arena || !g_slot_off || !g_slot_id) {
    std::printf("[TPROF] sin memoria para las tablas: apagado\n");
    return false;
  }
  g_base = image_base;
  g_dir = log_dir;
  std::remove((g_dir + "/tprof_stacks.bin").c_str());
  std::remove((g_dir + "/tprof_samples.bin").c_str());
  std::remove((g_dir + "/tprof_host.bin").c_str());
  if (FILE *f = std::fopen((g_dir + "/tprof_info.txt").c_str(), "w")) {
    std::fprintf(f, "tprof v1\nbuild %s\nbase 0x%llx\nrec_bytes %u\n", build,
                 (unsigned long long)image_base, (unsigned)sizeof(Rec));
    std::fclose(f);
  }
  g_on = true;
  std::printf("[TPROF] encendido: pila invitada por muestra -> %s/tprof_*.bin\n", log_dir);
  return true;
}

void sample(const CPUContext &c, uint64_t host_pc, uint64_t host_lr, uint32_t t_us,
            const uint64_t *host_ret, int n_host_ret) {
  if (!g_on) return;
  const uint32_t i = g_nrec.load(std::memory_order_relaxed);
  if (i >= kMaxRec) return;
  Rec &r = g_rec[i];
  r.t_us = t_us;
  r.pc = host_pc - g_base < 0xFFFFFFFFull ? (uint32_t)(host_pc - g_base) : 0xFFFFFFFFu;
  r.lr = host_lr - g_base < 0xFFFFFFFFull ? (uint32_t)(host_lr - g_base) : 0xFFFFFFFFu;
  r.frame = (uint16_t)g_guest_xfb.load(std::memory_order_relaxed);
  r.scene = (int8_t)g_tprof_scene.load(std::memory_order_relaxed);
  uint32_t e[kMaxDepth + 8];
  int n = 0;
  const uint32_t sp = *(const volatile uint32_t *)&c.gpr[1];
  e[n++] = *(const volatile uint32_t *)&c.pc;
  e[n++] = *(const volatile uint32_t *)&c.lr;
  uint32_t v = 0;
  e[n++] = rd32(c, sp + 4, v) ? v : 0;
  n = walk(c, sp, e, n);
  r.stack = intern(e, n);
  // Hilo principal del juego = el hilo invitado que mas veces sale como actual
  // (en NSMBW no es el hilo por defecto: dMain::main01 corre en uno creado aparte).
  uint32_t cur = 0, mainthr = 0;
  rd32(c, 0x800000E4, cur);
  {
    static uint32_t thr[8], cnt[8];
    int best = -1, slot = -1;
    for (int k = 0; k < 8; ++k) {
      if (cur && thr[k] == cur) slot = k;
      if (thr[k] && (best < 0 || cnt[k] > cnt[best])) best = k;
    }
    if (cur && slot < 0)
      for (int k = 0; k < 8; ++k)
        if (!thr[k]) { thr[k] = cur; slot = k; break; }
    if (slot >= 0) {
      ++cnt[slot];
      if (best < 0 || cnt[slot] > cnt[best]) best = slot;
    }
    if (best >= 0) mainthr = thr[best];
  }
  r.cur_thr = cur;
  r.flags = (cur == mainthr && cur) ? 1 : 0;
  r.stack2 = 0;
  if (cur != mainthr && mainthr) {
    uint32_t msp = 0, srr0 = 0, mlr = 0;
    if (rd32(c, mainthr + 4, msp) && rd32(c, mainthr + 0x198, srr0) && rd32(c, mainthr + 0x84, mlr)) {
      n = 0;
      e[n++] = srr0;
      e[n++] = mlr;
      e[n++] = rd32(c, msp + 4, v) ? v : 0;
      n = walk(c, msp, e, n);
      r.stack2 = intern(e, n);
    }
  }
  r.hstack = 0;
  if (host_ret && n_host_ret > 0) {
    n = 0;
    e[n++] = 0xFFFFFFFFu;  // marca: pila del anfitrion
    for (int k = 0; k < n_host_ret && k < 12; ++k)
      e[n++] = host_ret[k] - g_base < 0xFFFFFFFFull ? (uint32_t)(host_ret[k] - g_base) : 0xFFFFFFFEu;
    r.hstack = intern(e, n);
  }
  g_nrec.store(i + 1, std::memory_order_release);
}

void sample_host(int slot, uint64_t host_pc, uint64_t host_lr, uint32_t t_us,
                 const uint64_t *host_ret, int n_host_ret) {
  if (!g_on) return;
  const uint32_t i = g_nhrec.load(std::memory_order_relaxed);
  if (i >= kMaxRec) return;
  HRec &r = g_hrec[i];
  r.t_us = t_us;
  r.pc = host_pc - g_base < 0xFFFFFFFFull ? (uint32_t)(host_pc - g_base) : 0xFFFFFFFFu;
  r.lr = host_lr - g_base < 0xFFFFFFFFull ? (uint32_t)(host_lr - g_base) : 0xFFFFFFFFu;
  r.frame = (uint16_t)g_guest_xfb.load(std::memory_order_relaxed);
  r.scene = (int8_t)g_tprof_scene.load(std::memory_order_relaxed);
  r.slot = (uint8_t)slot;
  r.hstack = 0;
  if (host_ret && n_host_ret > 0) {
    uint32_t e[16];
    int n = 0;
    e[n++] = 0xFFFFFFFFu;
    for (int k = 0; k < n_host_ret && k < 12; ++k)
      e[n++] = host_ret[k] - g_base < 0xFFFFFFFFull ? (uint32_t)(host_ret[k] - g_base) : 0xFFFFFFFEu;
    r.hstack = intern(e, n);
  }
  g_nhrec.store(i + 1, std::memory_order_release);
}

void flush() {
  if (!g_on) return;
  // Primero cuantas muestras hay y despues cuantas pilas: asi toda muestra escrita
  // tiene ya sus pilas en el archivo.
  const uint32_t nr = g_nrec.load(std::memory_order_acquire);
  const uint32_t nh = g_nhrec.load(std::memory_order_acquire);
  const uint32_t au = g_arena_used.load(std::memory_order_acquire);
  if (au > g_arena_written)
    if (FILE *f = std::fopen((g_dir + "/tprof_stacks.bin").c_str(), "ab")) {
      std::fwrite(g_arena + g_arena_written, 4, au - g_arena_written, f);
      std::fclose(f);
      g_arena_written = au;
    }
  if (nh > g_hrec_written)
    if (FILE *f = std::fopen((g_dir + "/tprof_host.bin").c_str(), "ab")) {
      std::fwrite(g_hrec + g_hrec_written, sizeof(HRec), nh - g_hrec_written, f);
      std::fclose(f);
      g_hrec_written = nh;
    }
  if (nr > g_rec_written)
    if (FILE *f = std::fopen((g_dir + "/tprof_samples.bin").c_str(), "ab")) {
      std::fwrite(g_rec + g_rec_written, sizeof(Rec), nr - g_rec_written, f);
      std::fclose(f);
      g_rec_written = nr;
    }
}

#ifndef __SWITCH__
namespace {
uint64_t g_t0_ns = 0;
uint64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
void on_sig(int, siginfo_t *, void *uc_) {
  const CPUContext *c = g_ctx_ptr;
  if (!c) return;
  const ucontext_t *uc = (const ucontext_t *)uc_;
#if defined(__x86_64__)
  const uint64_t pc = (uint64_t)uc->uc_mcontext.gregs[REG_RIP];
#else
  const uint64_t pc = 0;
#endif
  sample(*c, pc, 0, (uint32_t)((now_ns() - g_t0_ns) / 1000), nullptr, 0);
}
}  // namespace

void attach_cpu_thread() {
  if (!init(".", (uint64_t)(uintptr_t)&__executable_start, __DATE__ " " __TIME__)) return;
  const char *d = std::getenv("NWII_TPROF_DIR");
  if (d && *d) {
    g_dir = d;
    std::remove((g_dir + "/tprof_stacks.bin").c_str());
    std::remove((g_dir + "/tprof_samples.bin").c_str());
    if (FILE *f = std::fopen((g_dir + "/tprof_info.txt").c_str(), "w")) {
      std::fprintf(f, "tprof v1\nbuild %s\nbase 0x%llx\nrec_bytes %u\n", __DATE__ " " __TIME__,
                   (unsigned long long)g_base, (unsigned)sizeof(Rec));
      std::fclose(f);
    }
  }
  g_t0_ns = now_ns();
  struct sigaction sa = {};
  sa.sa_sigaction = on_sig;
  sa.sa_flags = SA_SIGINFO | SA_RESTART;
  sigaction(SIGRTMIN + 5, &sa, nullptr);
  sigevent sev = {};
  sev.sigev_notify = SIGEV_THREAD_ID;
  sev.sigev_signo = SIGRTMIN + 5;
  sev._sigev_un._tid = (pid_t)syscall(SYS_gettid);
  timer_t tm;
  if (timer_create(CLOCK_MONOTONIC, &sev, &tm) != 0) {
    std::printf("[TPROF] timer_create fallo\n");
    return;
  }
  const char *us_s = std::getenv("NWII_TPROF_US");
  const long us = us_s ? std::max(100, std::atoi(us_s)) : 1000;
  itimerspec its = {};
  its.it_interval.tv_nsec = us * 1000;
  its.it_value.tv_nsec = us * 1000;
  timer_settime(tm, 0, &its, nullptr);
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(2));
      flush();
    }
  }).detach();
  std::printf("[TPROF] PC: senal cada %ld us al hilo de la CPU\n", us);
}
#else
void attach_cpu_thread() {}
#endif

}  // namespace tprof
}  // namespace nwii::runtime
