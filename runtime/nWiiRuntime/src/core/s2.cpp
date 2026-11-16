// r34s2 (frente S2): "CPU de sonido". Ver s2.h y r9/r34s2/LEEME.md (diseno y razonamiento).
//
// Resumen de la sincronizacion (todo lo demas del SO invitado sigue siendo de un solo procesador):
//  S   candado anfitrion que sustituye a "interrupciones apagadas" ENTRE las dos CPU para el estado
//      de sonido. CPU de sonido: lo tiene siempre que su MSR[EE] = 0 (todo manejador de interrupcion
//      y toda seccion OSDisableInterrupts del SoundThread). CPU del juego: lo toma en las secciones
//      OSDisableInterrupts abiertas desde codigo que no es SDK-no-sonido (clasificado por la direccion
//      de retorno) y durante todo manejador de interrupcion / callback; lo suelta cuando EE vuelve a 1
//      o cuando el hilo invitado duerme con la seccion abierta (y lo retoma al reanudarse ese hilo).
//  M   los OSMutex que usa el sonido (el de SoundThread): la CPU de sonido no toca la estructura del
//      mutex; pide tomar/soltar a la CPU del juego, que lo hace en nativo en un sondeo con EE = 1 y a
//      nivel de hilo (atomico respecto a todas las secciones criticas del SO invitado), a nombre del
//      OSThread SoundThread de verdad (que sigue dormido en su cola). Despertar hilos (OSWakeupThread)
//      va igual: la CPU del juego ejecuta el OSWakeupThread invitado como un callback mas.
#include "s2.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include "runtime/nwii_env.h"
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <pthread.h>
// ThreadSanitizer (TSAN=1): el longjmp de la fibra sale desde codigo generado sin instrumentar; se usa el
// longjmp de libc y el camino (snd_run_guest, snd_os, nwii_s2_os) no se instrumenta (pila sombra equilibrada),
// igual que NWII_LONGJMP en ios.cpp.
#if defined(__SANITIZE_THREAD__)
#include <dlfcn.h>
#define S2_NO_TSAN __attribute__((no_sanitize("thread")))
[[noreturn]] S2_NO_TSAN static void s2_longjmp(jmp_buf &b) {
  static void (*real)(jmp_buf, int) = (void (*)(jmp_buf, int))dlsym(dlopen("libc.so.6", RTLD_NOW), "longjmp");
  real(b, 1);
  __builtin_unreachable();
}
#else
#define S2_NO_TSAN
#define s2_longjmp(b) longjmp(b, 1)
#endif
#ifdef __SWITCH__
#include <switch.h>
#include "../platform/switch/switch_platform.h"
#endif

// Tabla de despacho del codigo generado (fastdispatch.cpp; functions.h: base 0x80004000). run_game no
// sirve para el contexto de sonido: escribe su historial de pc en el registro cada vez que termina.
typedef void (*nwii_fn)(nwii::runtime::CPUContext &);
extern nwii_fn *g_disp_table __attribute__((visibility("hidden")));
extern "C" { extern bool nwii_s2_on; }

namespace nwii::runtime {
extern CPUContext *g_ctx_ptr;
extern bool g_fm_alias_next;
extern std::atomic<uint64_t> g_null_calls;      // os_hle_stubs.cpp (r38c)
extern std::atomic<uint32_t> g_null_last_site;  // fastmem_impl.inc: el siguiente MMU no reserva memoria (se le dan los bloques del principal)
namespace s2 {

bool g_enabled = false;
std::atomic<bool> g_active{false};
std::atomic<uint32_t> g_req{0};
std::atomic<uint32_t> g_game_work{0};

namespace {

// ---- direcciones (SMNE01, USA) ------------------------------------------------------------
// Hilos invitados que corre la CPU de sonido ("fibras"): su cuerpo es `for (;;) { OSReceiveMessage(cola,
// &msg, BLOCK); trabajo(msg); }` y los despierta un callback de AX. Se identifican por la direccion de
// retorno de ese OSReceiveMessage; cada vuelta se reanuda ahi con los registros que tenia el hilo de verdad.
constexpr uint32_t kRecvRetSnd = 0x8027B9A0;    // nw4r::snd::detail::SoundThread::SoundThreadProc
constexpr uint32_t kRecvRetGame = 0x80174598;   // hilo de sonido del juego (func_80174560; su callback AX: 0x801744F0)
constexpr uint32_t kOSWakeupThread = 0x801B5D80;
constexpr uint32_t kCurThread = 0x800000E4;
enum : uint32_t {
  F_OSLockMutex = 0x801B2E60, F_OSUnlockMutex = 0x801B2F40, F_OSTryLockMutex = 0x801B3080,
  F_OSSendMessage = 0x801B23F0, F_OSReceiveMessage = 0x801B24C0, F_OSSleepThread = 0x801B5C90,
  F_OSWakeupThread = 0x801B5D80, F_OSSetCurrentContext = 0x801ACD40, F_OSGetCurrentThread = 0x801B4B30,
  F_OSYieldThread = 0x801B50F0,
};
// OSThread / OSMutex / OSMessageQueue (Revolution SDK; comprobado contra el codigo de OSLockMutex)
constexpr uint32_t kThState = 0x2C8, kThPrio = 0x2D0, kThBase = 0x2D4, kThQueue = 0x2DC, kThMtxHead = 0x2F4,
                   kThMtxTail = 0x2F8, kThStackEnd = 0x308;
constexpr uint32_t kMtxThread = 8, kMtxCount = 12, kMtxNext = 16, kMtxPrev = 20;
constexpr uint32_t kMqRecvQueue = 8, kMqUsed = 0x1C;

inline void cpu_relax() {
#if defined(__x86_64__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  __asm__ volatile("yield");
#endif
}
inline uint64_t now_ns() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool g_check = false;   // NWII_S2_CHECK=1: asserts de propiedad
bool g_log = false;     // NWII_S2_LOG=1
thread_local bool t_snd = false;

CPUContext *g_main = nullptr;
CPUContext *g_snd = nullptr;

// ---- estadisticas -------------------------------------------------------------------------
struct Stats {
  std::atomic<uint64_t> takeovers{0}, handbacks{0}, irq_aid{0}, irq_dsp{0}, frames{0}, msg_drop{0};
  std::atomic<uint64_t> req_lock{0}, req_lock_fail{0}, req_unlock{0}, req_wake{0}, req_self{0}, req_wait_ns{0}, req_send{0};
  std::atomic<uint64_t> g_sec{0}, g_irq{0}, g_wait_ns{0}, g_wait_n{0}, g_stash{0};
  std::atomic<uint64_t> s_acq{0}, s_wait_ns{0}, s_wait_n{0}, naps{0}, mtx_waits{0}, mtx_in_sec{0};
  std::atomic<uint64_t> traps{0}, checks_bad{0}, irq_ns{0}, frame_ns{0}, f2_ns{0}, late_max_us{0}, wakes_cb{0};
} st;
inline void add(std::atomic<uint64_t> &c, uint64_t v = 1) { c.store(c.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); }

// ---- r43a (frente A): coste del hilo de sonido. Contadores (linea [A43]) e interruptores (valor != "0"):
//   NWII_A_NOSPIN=1   esperas cortas (< 50 us hasta el siguiente evento) DURMIENDO (svcSleepThread) en vez de
//                     ceder en bucle (en Horizon ceder no suelta el nucleo a los de menos prioridad: el trabajador)
//   NWII_A_REQSPIN=n  vueltas de giro de una peticion al juego antes de empezar a dormir (por defecto 1500)
std::atomic<uint64_t> st_a_os{0};
std::atomic<uint64_t> st_a_cvwait{0}, st_a_yield{0}, st_a_nap{0}, st_a_polls{0}, st_a_dsp_ns{0}, st_a_req_spin{0}, st_a_req_sleep{0};
bool g_a_nospin = false;
uint32_t g_a_reqspin = 1500;

void check_fail(const char *what, uint32_t a = 0, uint32_t b = 0) {
  const uint64_t n = st.checks_bad.fetch_add(1, std::memory_order_relaxed);
  if (n < 40) std::printf("[S2] ASSERT: %s (0x%08x, 0x%08x)%s\n", what, a, b, t_snd ? " [hilo de sonido]" : " [hilo del juego]");
}

// ---- r38c (frente C): esperas activas que ceden de verdad ----------------------------------
// En Horizon ceder (sched_yield = svcSleepThread(-1)) solo deja correr a hilos de la MISMA prioridad. Si la
// CPU de sonido (0x2C, nucleo 2) gira esperando S mientras el juego, con S tomado, espera un candado que tiene
// un hilo de menos prioridad de su nucleo (el trabajador de render, 0x2D, con g_fifo_mutex), nadie avanza:
// cuelgue de Z22. Aqui, tras un giro corto, se DUERME (svcSleepThread > 0): el nucleo pasa a los de abajo.
// NWII_C_SPIN_OFF=1: como m5 (yield).
bool g_spin_old = false;
std::atomic<uint32_t> g_snd_wait{0};      // 0 nada, 1 candado S, 2 peticion al juego, 3 candado DSP, 4 siesta corta
std::atomic<uint64_t> g_snd_wait_t0{0};   // ns
std::atomic<uint64_t> g_game_wait_t0{0};  // el juego espera S desde (ns; 0 = no espera)
std::atomic<uint64_t> st_backoff_sleeps{0}, st_game_wait_max_us{0}, st_snd_wait_max_us{0};
uint32_t gs_acq_lr = 0, gs_acq_site = 0;  // juego: de donde tomo S (informativo, vigia)
inline void sleep_us(uint32_t us) {
#ifdef __SWITCH__
  svcSleepThread((int64_t)us * 1000);
#else
  std::this_thread::sleep_for(std::chrono::microseconds(us));
#endif
}
inline void backoff(uint32_t &spins) {
  ++spins;
  if (g_spin_old) {
    cpu_relax();
    if (spins > 4000) std::this_thread::yield();
    return;
  }
  if (spins < 2000) { cpu_relax(); return; }
  sleep_us(spins < 2100 ? 20 : 200);
  st_backoff_sleeps.fetch_add(1, std::memory_order_relaxed);
}
inline void note_max(std::atomic<uint64_t> &m, uint64_t v) {
  if (v > m.load(std::memory_order_relaxed)) m.store(v, std::memory_order_relaxed);
}

// ---- r38c: lista AX de SoundThread (segundo cuelgue, D07/m6a) --------------------------------------
// SoundThread::AxCallbackFunc (0x8027B850) corre dentro de la interrupcion AID (CPU de sonido, con S) y recorre la lista
// +0x388 del objeto 0x80412BF0 llamando al virtual +0x10 de cada nodo, con el siguiente nodo ya leido. El juego da de
// alta / de baja nodos con SoundThread::Register (0x8027BA20) / Unregister (0x8027BA90) protegido SOLO por el mutex
// del SoundThread (+0x354): en la Wii basta, porque la interrupcion nunca corre a la vez que un hilo. Con S2 el juego
// puede quitar (y liberar / reutilizar) el nodo siguiente mientras la CPU de sonido lo recorre -> lista rota.
// Arreglo (NWII_C_AXLIST, encendido; "off"/cualquier valor != "1" no sirve: apagar con NWII_C_AXLIST_OFF=1): el juego toma S
// (kHoldList) desde que tiene el mutex en Register/Unregister hasta que lo suelta, igual que si la interrupcion no
// pudiera entrar. NWII_C_AXLIST_CHECK=1: la CPU de sonido valida la lista al final de cada interrupcion.
constexpr uint32_t kAxListObj = 0x80412BF0, kAxListHead = kAxListObj + 0x388;
constexpr uint32_t kRegLockLr = 0x8027BA4C, kRegUnlockLr = 0x8027BA6C, kUnregLockLr = 0x8027BABC, kUnregUnlockLr = 0x8027BAD0;
bool g_axl_fix = true, g_axl_check = false;
bool g_axl_mutex = false;  // NWII_C_AXLIST_MUTEX=1: regla general (S mientras el juego tenga el mutex del SoundThread, en todo uso)
constexpr uint32_t kSndMutex = kAxListObj + 0x354;
std::atomic<uint64_t> st_axl_hold_ns{0}, st_axl_hold_max_us{0};
uint64_t g_axl_t_lock = 0;
std::atomic<int> g_snd_in_irq{0};
std::atomic<uint64_t> st_axl_calls{0}, st_axl_overlap{0}, st_axl_fallback{0}, st_axl_bad{0}, st_axl_wait_ns{0};
// r38c: ultimas altas/bajas en la lista (para el informe de NWII_C_AXLIST_CHECK)
struct AxlEv { uint64_t t; uint32_t node, thread, lr; uint8_t side, s_held, snd_irq; };
AxlEv g_axl_ring[32];
std::atomic<uint32_t> g_axl_ri{0};
inline void axl_note(uint32_t node, uint32_t thread, uint32_t lr, uint8_t side, uint8_t s_held, uint8_t snd_irq) {
  const uint32_t i = g_axl_ri.fetch_add(1, std::memory_order_relaxed) & 31;
  g_axl_ring[i] = {now_ns(), node, thread, lr, side, s_held, snd_irq};
}
void axl_ring_dump() {
  const uint32_t e = g_axl_ri.load();
  const uint64_t now = now_ns();
  for (uint32_t k = e > 32 ? e - 32 : 0; k < e; ++k) {
    const AxlEv &v = g_axl_ring[k & 31];
    std::printf("[C38]   hace %8.3f ms: %s %s nodo 0x%08x hilo 0x%08x (S del juego %d, sonido en interrupcion %d)\n", (now - v.t) / 1e6,
                v.side ? "SONIDO" : "juego ", (v.lr == 0x8027BA4C) ? "alta" : (v.lr == 0x8027BABC) ? "baja" : "?   ", v.node, v.thread, v.s_held, v.snd_irq);
  }
}
// r38c NWII_C_AXLIST_CHECK: la lista +0x388 debe volver al centinela con enlaces coherentes y tablas virtuales.
void axlist_check(const char *where) {
  uint32_t n = kAxListHead;
  for (int i = 0; i < 64; ++i) {
    const uint32_t nx = g_main->mmu.read32(n);
    if (nx == kAxListHead) return;
    const bool in = (nx >= 0x80000000u && nx < 0x81800000u) || (nx >= 0x90000000u && nx < 0x94000000u);
    if (!in || g_main->mmu.read32(nx + 4) != n || g_main->mmu.read32(nx + 8) == 0) {
      if (st_axl_bad.load() == 0) axl_ring_dump();
      if (st_axl_bad.fetch_add(1) < 10)
        std::printf("[C38] LISTA AX ROTA (%s): nodo 0x%08x -> 0x%08x, paso %d; centinela 0x%08x/0x%08x cuenta %u; nodo: %08x %08x %08x %08x %08x %08x\n",
                    where, n, nx, i, g_main->mmu.read32(kAxListHead), g_main->mmu.read32(kAxListHead + 4), g_main->mmu.read32(kAxListHead - 4),
                    g_main->mmu.read32(nx), g_main->mmu.read32(nx + 4), g_main->mmu.read32(nx + 8), g_main->mmu.read32(nx + 12),
                    g_main->mmu.read32(nx + 16), g_main->mmu.read32(nx + 20));
      return;
    }
    n = nx;
  }
  if (st_axl_bad.fetch_add(1) < 10) std::printf("[C38] LISTA AX ROTA (%s): mas de 64 nodos sin volver al centinela\n", where);
}
// ---- r38c: prioridad dinamica de la CPU de sonido ------------------------------------------
// Base NWII_S2_PRIO (p. ej. 0x2E, por debajo del trabajador de render 0x2D); sube a NWII_C_PRIO_HI (p. ej. 0x2C) solo
// mientras haga falta: bit 1 = va tarde (un evento de su agenda vencido hace mas de NWII_C_LATE_US), bit 2 = el juego
// espera S y lo tiene el sonido (herencia), bit 4 = un hilo del juego va a dormir en un mutex del sonido (herencia).
// Sin NWII_C_PRIO_HI no cambia nada (como m5).
int g_prio_lo = 0x2C, g_prio_hi = 0;
uint64_t g_late_boost_us = 3000;
std::atomic<uint32_t> g_boost{0};
std::atomic<int> g_prio_now{0};
std::atomic<uint64_t> st_boost_n[3] = {}, st_boost_ns{0};
std::atomic<uint64_t> g_boost_t0{0};
#ifndef __SWITCH__
pthread_t g_snd_pthread{};
#endif
void set_snd_prio(int p) {
  if (g_prio_now.load(std::memory_order_relaxed) == p) return;
  g_prio_now.store(p, std::memory_order_relaxed);
#ifdef __SWITCH__
  if (const Handle h = nx::thread_handle(nx::kThreadS2)) svcSetThreadPriority(h, p);
#else
  if (std::getenv("NWII_C_PCSCHED")) {
    sched_param sp{};
    sp.sched_priority = std::max(1, 0x3C - p);
    pthread_setschedparam(g_snd_pthread, SCHED_FIFO, &sp);
  }
#endif
}
inline void boost(uint32_t bit) {  // cualquier hilo
  if (!g_prio_hi) return;
  const uint32_t old = g_boost.fetch_or(bit, std::memory_order_acq_rel);
  if (!(old & bit)) st_boost_n[bit == 1 ? 0 : bit == 2 ? 1 : 2].fetch_add(1, std::memory_order_relaxed);
  if (old == 0) { g_boost_t0.store(now_ns(), std::memory_order_relaxed); set_snd_prio(g_prio_hi); }
}
inline void unboost(uint32_t bits) {  // solo el hilo de sonido
  if (!g_prio_hi || !(g_boost.load(std::memory_order_relaxed) & bits)) return;
  if ((g_boost.fetch_and(~bits, std::memory_order_acq_rel) & ~bits) != 0) return;
  const uint64_t t0 = g_boost_t0.load(std::memory_order_relaxed);
  set_snd_prio(g_prio_lo);
  if (g_boost.load(std::memory_order_acquire) != 0) set_snd_prio(g_prio_hi);  // alguien la pidio entretanto
  else if (t0) st_boost_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
}

// ---- candado S ----------------------------------------------------------------------------
std::atomic<uint32_t> s_lock{0};  // 0 libre, 1 juego, 2 sonido
enum : uint32_t { kHoldNone = 0, kHoldSection = 1, kHoldIrq = 2, kHoldList = 3 /* r38c: alta/baja en la lista AX de SoundThread */ };
struct GameS {  // solo el hilo del juego
  uint32_t holds = kHoldNone, owner = 0;
  uint32_t list_owner = 0;  // r38c: hilo invitado dentro de SoundThread::Register/Unregister (kHoldList)
  uint32_t stash[8] = {0};
  int nstash = 0;
} gs;
bool snd_holds = false;  // solo el hilo de sonido

// ---- peticiones de la CPU de sonido a la del juego ----------------------------------------
enum : uint32_t { kReqLock = 1, kReqUnlock = 2, kReqWake = 3, kReqSend = 4 };
struct Req { uint32_t op = 0, arg = 0, arg2 = 0, result = 0; } rq;
std::atomic<uint32_t> g_park{0};  // 0 el juego corre, 1 el juego duerme en el bucle ocioso, 2 sonido atendiendo
std::mutex idle_mx;
std::condition_variable idle_cv;
bool idle_wake = false;

// ---- estado de la CPU de sonido -----------------------------------------------------------
struct Fiber {
  const char *name;
  uint32_t recv_ret;
  // captura: escrita por el hilo del juego antes de poner hosted
  bool cap_ok = false;
  uint32_t queue = 0, thread = 0, out_ptr_off = 0, fsz = 0, sp = 0, stack_end = 0, qcap = 4;
  std::array<uint32_t, 32> gpr0{};
  std::atomic<bool> hosted{false};  // la CPU de sonido corre este hilo (el de verdad duerme en su cola)
  // solo el hilo de sonido
  uint32_t msgs[8] = {0};
  int head = 0, n = 0;
  jmp_buf jb;
};
Fiber fibers[2] = {{"nw4r SoundThread", kRecvRetSnd}, {"hilo de sonido del juego", kRecvRetGame}};
constexpr int kNumFibers = 2;
Fiber *cur_fiber = nullptr;  // fibra en ejecucion (hilo de sonido)
bool g_fiber2 = true;        // NWII_S2_F2=0: el segundo hilo se queda en la CPU del juego
enum : uint32_t { kParked = 0, kRunning = 1, kQuit = 2 };
std::atomic<uint32_t> snd_state{kParked};
std::atomic<bool> hb_req{false};
std::atomic<bool> snd_exited{false};
std::mutex snd_mx;
std::condition_variable snd_cv;
bool snd_go = false;
jmp_buf snd_exit_jb;
inline Fiber *fiber_of_queue(uint32_t q) {
  for (Fiber &f : fibers) if (f.queue == q && f.hosted.load(std::memory_order_acquire)) return &f;
  return nullptr;
}
uint32_t snd_stack_end = 0;  // limite inferior de la pila en uso (para el aviso de pila)
int in_irq = 0;
struct MtxDepth { uint32_t m; int depth; };
std::vector<MtxDepth> snd_mtx;
uint64_t snd_slip = 0;  // reloj de sonido = reloj de pared - snd_slip (igual al del juego en la toma)

// agenda de la CPU de sonido (solo su hilo; el del juego la toca con la CPU de sonido aparcada)
struct Ev { uint64_t due, id; Cb cb; };
std::vector<Ev> evs;
uint64_t ev_next_id = 1;

// ---- utilidades de memoria invitada -------------------------------------------------------
inline uint32_t rd32(uint32_t a) { return g_main->mmu.read32(a); }
inline void wr32(uint32_t a, uint32_t v) { g_main->mmu.write32(a, v); }

inline bool snd_class(uint32_t lr) {
  // Direccion de retorno de quien apaga interrupciones: todo menos SDK/nw4r que no es de sonido.
  if (lr - 0x801A9AA0u < 0x801D5600u - 0x801A9AA0u) return false;  // base, OS, EXI/SI/VI, MTX, GX, DVD...
  if (lr - 0x801D6400u < 0x80260000u - 0x801D6400u) return false;  // MEM/NAND/SC/WPAD/KPAD..., nw4r::ut/math/g3d
  if (lr - 0x80285800u < 0x802B8A70u - 0x80285800u) return false;  // nw4r::ef / lyt
  if (lr - 0x802DC000u < 0x80300000u - 0x802DC000u) return false;  // MSL / runtime
  if (lr - 0x80004000u < 0x80006780u - 0x80004000u) return false;
  return true;
}

// ---- servicio de peticiones (hilo del juego, o el de sonido con el juego aparcado) --------
void wake_game_idle() {
  { std::lock_guard<std::mutex> lk(idle_mx); idle_wake = true; }
  idle_cv.notify_one();
}

uint32_t do_trylock(uint32_t m, uint32_t th) {
  const uint32_t owner = rd32(m + kMtxThread);
  if (owner == 0) {
    wr32(m + kMtxThread, th);
    wr32(m + kMtxCount, rd32(m + kMtxCount) + 1);
    const uint32_t tail = rd32(th + kThMtxTail);
    if (tail == 0) wr32(th + kThMtxHead, m); else wr32(tail + kMtxNext, m);
    wr32(m + kMtxPrev, tail);
    wr32(m + kMtxNext, 0);
    wr32(th + kThMtxTail, m);
    return 1;
  }
  if (owner == th) { wr32(m + kMtxCount, rd32(m + kMtxCount) + 1); return 1; }
  return 0;
}

void do_wake(CPUContext &ctx, uint32_t q) {
  if (rd32(q) == 0) return;  // nadie espera
  ctx.queue_callback(kOSWakeupThread, q, 0);
  add(st.wakes_cb);
  ctx.poll_kick();
  wake_game_idle();
}

void do_unlock(CPUContext &ctx, uint32_t m, uint32_t th) {
  if (rd32(m + kMtxThread) != th) { check_fail("unlock de un mutex que no es del sonido", m, rd32(m + kMtxThread)); return; }
  const uint32_t c = rd32(m + kMtxCount) - 1;
  wr32(m + kMtxCount, c);
  if (c != 0) return;
  const uint32_t next = rd32(m + kMtxNext), prev = rd32(m + kMtxPrev);
  if (next == 0) wr32(th + kThMtxTail, prev); else wr32(next + kMtxPrev, prev);
  if (prev == 0) wr32(th + kThMtxHead, next); else wr32(prev + kMtxNext, next);
  wr32(m + kMtxThread, 0);
  // Herencia de prioridad: si un hilo mas prioritario espero este mutex subio la prioridad del SoundThread
  // (que duerme solo en su cola: no hay que reordenar nada).
  if (rd32(th + kThPrio) < rd32(th + kThBase) && rd32(th + kThMtxHead) == 0) wr32(th + kThPrio, rd32(th + kThBase));
  do_wake(ctx, m);  // OSWakeupThread(&mutex->queue)
}

// OSSendMessage(mq, msg, NOBLOCK) sobre una cola de un hilo del juego, sin el despertar (ver snd_wake).
uint32_t do_send(uint32_t mq, uint32_t msg) {
  if (fiber_of_queue(mq)) return 2;  // entretanto la CPU de sonido se quedo con ese hilo: lo encola ella
  const uint32_t cnt = rd32(mq + 0x14), used = rd32(mq + kMqUsed);
  if (cnt == 0 || used >= cnt) return 0;
  const uint32_t idx = (rd32(mq + 0x18) + used) % cnt;
  wr32(rd32(mq + 0x10) + idx * 4, msg);
  wr32(mq + kMqUsed, used + 1);
  return 1;
}

void service(CPUContext &ctx) {
  if (g_req.load(std::memory_order_acquire) != 1) return;
  switch (rq.op) {
    case kReqLock: rq.result = do_trylock(rq.arg, rq.arg2); break;
    case kReqUnlock: do_unlock(ctx, rq.arg, rq.arg2); rq.result = 1; break;
    case kReqWake: do_wake(ctx, rq.arg); rq.result = 1; break;
    case kReqSend: rq.result = do_send(rq.arg, rq.arg2); break;
  }
  g_req.store(2, std::memory_order_release);
}

inline bool game_can_service(const CPUContext &c) {
  return (c.msr & 0x8000) && !c.in_callback && c.dispatch_saved_ctx == 0;
}

// ---- candado S, lado del juego ------------------------------------------------------------
void game_acquire(CPUContext &ctx, uint32_t kind, uint32_t owner, bool can_service) {
  uint32_t exp = 0;
  if (!s_lock.compare_exchange_strong(exp, 1, std::memory_order_acquire, std::memory_order_relaxed)) {
    const uint64_t t0 = now_ns();
    g_game_wait_t0.store(t0, std::memory_order_relaxed);
    uint32_t spins = 0;
    for (;;) {
      exp = 0;
      if (s_lock.compare_exchange_weak(exp, 1, std::memory_order_acquire, std::memory_order_relaxed)) break;
      if (g_check && exp == 1) { check_fail("el juego ya tenia S"); }
      if (exp == 2 && spins == 64) boost(2);  // r38c: herencia (la CPU de sonido tiene S y puede estar expulsada)
      if (can_service && g_req.load(std::memory_order_relaxed) == 1) service(ctx);
      cpu_relax();
      if (++spins > 4000) { std::this_thread::yield(); }
    }
    g_game_wait_t0.store(0, std::memory_order_relaxed);
    const uint64_t w = now_ns() - t0;
    note_max(st_game_wait_max_us, w / 1000);
    add(st.g_wait_ns, w);
    add(st.g_wait_n);
  }
  gs.holds = kind;
  gs.owner = owner;
  gs_acq_lr = ctx.lr;
  gs_acq_site = ctx.pc;
  g_game_work.store(g_game_work.load(std::memory_order_relaxed) | 2, std::memory_order_relaxed);
  add(kind == kHoldIrq ? st.g_irq : st.g_sec);
}
inline void game_release() {
  gs.holds = kHoldNone;
  s_lock.store(0, std::memory_order_release);
  g_game_work.store(g_game_work.load(std::memory_order_relaxed) & ~2u, std::memory_order_relaxed);
}
inline bool stash_take(uint32_t th) {
  for (int i = 0; i < gs.nstash; ++i)
    if (gs.stash[i] == th) { gs.stash[i] = gs.stash[--gs.nstash]; return true; }
  return false;
}
// Reglas del lado del juego tras cualquier cambio de MSR[EE] o de hilo invitado.
inline void game_rules(CPUContext &ctx, bool sec_open, bool can_service) {
  const bool ee = ctx.msr & 0x8000;
  const uint32_t cur = rd32(kCurThread);
  if (gs.holds) {
    if (cur != gs.owner) {
      // El hilo invitado que tenia la seccion ha dejado la CPU con ella abierta (OSSleepThread): el
      // candado se suelta y ese hilo lo retoma al reanudarse. Una interrupcion no deja nada pendiente.
      // (kHoldList: lo retoma su hilo, gs.list_owner, al volver a correr; ver abajo.)
      if (gs.holds == kHoldSection && gs.owner != 0) {
        if (gs.nstash < 8) gs.stash[gs.nstash++] = gs.owner; else check_fail("stash lleno");
        add(st.g_stash);
      }
      game_release();
    } else if (ee && gs.holds != kHoldList) {
      game_release();
    }
  }
  if (!gs.holds && gs.list_owner && gs.list_owner == cur) game_acquire(ctx, kHoldList, cur, true);  // r38c
  if (!gs.holds && !ee) {
    if (gs.nstash && stash_take(cur)) game_acquire(ctx, kHoldSection, cur, true);
    else if (sec_open) game_acquire(ctx, kHoldSection, cur, can_service);
  }
}

// ---- candado S, lado del sonido -----------------------------------------------------------
inline void snd_ee_sync(CPUContext &ctx) {
  const bool ee = ctx.msr & 0x8000;
  if (!ee && !snd_holds) {
    uint32_t exp = 0;
    if (!s_lock.compare_exchange_strong(exp, 2, std::memory_order_acquire, std::memory_order_relaxed)) {
      const uint64_t t0 = now_ns();
      g_snd_wait_t0.store(t0, std::memory_order_relaxed);
      g_snd_wait.store(1, std::memory_order_relaxed);
      uint32_t spins = 0;
      for (;;) {
        exp = 0;
        if (s_lock.compare_exchange_weak(exp, 2, std::memory_order_acquire, std::memory_order_relaxed)) break;
        backoff(spins);
      }
      g_snd_wait.store(0, std::memory_order_relaxed);
      const uint64_t w = now_ns() - t0;
      note_max(st_snd_wait_max_us, w / 1000);
      add(st.s_wait_ns, w);
      add(st.s_wait_n);
    }
    snd_holds = true;
    add(st.s_acq);
  } else if (ee && snd_holds) {
    snd_holds = false;
    s_lock.store(0, std::memory_order_release);
    unboost(2);
  }
}

// ---- agenda y reloj de la CPU de sonido ---------------------------------------------------
void snd_run_irq(CPUContext &ctx, int n);
uint32_t request(CPUContext &ctx, uint32_t op, uint32_t arg, uint32_t arg2 = 0);
std::vector<uint32_t> deferred_wakes;

// depuracion (NWII_S2_F2DBG >= 1): histograma del hilo invitado actual de la CPU del juego, muestreado
// desde el hilo de sonido en cada vuelta de su planificador
struct CurSample { uint32_t th; uint32_t n; };
CurSample g_cur_hist[16];
void sample_cur_thread() {
  const uint32_t th = g_main->mmu.read32(0x800000E4);
  for (CurSample &c : g_cur_hist) {
    if (c.th == th) { ++c.n; return; }
    if (c.n == 0) { c.th = th; c.n = 1; return; }
  }
}
extern int g_f2dbg;
void snd_pump(CPUContext &ctx) {
  if (g_f2dbg) sample_cur_thread();
  // eventos vencidos (pueden levantar interrupciones)
  for (;;) {
    const uint64_t now = snd_now();
    size_t best = evs.size();
    for (size_t i = 0; i < evs.size(); ++i)
      if (evs[i].due <= now && (best == evs.size() || evs[i].due < evs[best].due ||
                                (evs[i].due == evs[best].due && evs[i].id < evs[best].id)))
        best = i;
    if (best == evs.size()) break;
    Ev e = std::move(evs[best]);
    evs.erase(evs.begin() + best);
    const uint64_t late = now - e.due;
    const uint64_t late_us = late * 1000000 / ctx.tb_freq;
    if (late_us > st.late_max_us.load(std::memory_order_relaxed)) st.late_max_us.store(late_us, std::memory_order_relaxed);
    if (late_us > g_late_boost_us) boost(1);  // r38c
    e.cb(ctx, late);
  }
  // interrupciones de sonido, solo con EE = 1 (como en la consola)
  while (ctx.msr & 0x8000) {
    const int n = hw::s2_dsp_irq();
    if (!n) break;
    snd_run_irq(ctx, n);
  }
}

uint64_t next_due() {
  uint64_t d = UINT64_MAX;
  for (const Ev &e : evs) d = std::min(d, e.due);
  return d;
}

// Espera del hilo de sonido hasta `due` (reloj de sonido) o hasta que lo avisen.
void snd_sleep_until(CPUContext &ctx, uint64_t due) {
  const uint64_t now = snd_now();
  if (due <= now) return;
  uint64_t us = (due - now) * 1000000 / ctx.tb_freq;
  if (us > 2000) us = 2000;
  if (us < 50) {
    if (g_a_nospin) { add(st_a_nap); sleep_us((uint32_t)us); return; }  // r43a
    add(st_a_yield);
    std::this_thread::yield();
    return;
  }
  add(st_a_cvwait);
  std::unique_lock<std::mutex> lk(snd_mx);
  if (hb_req.load(std::memory_order_relaxed) || snd_state.load(std::memory_order_relaxed) == kQuit) return;
  snd_cv.wait_for(lk, std::chrono::microseconds(us - 30));
}

struct Regs {
  std::array<uint32_t, 32> gpr; std::array<double, 32> fpr, ps1; std::array<ConditionField, 8> cr;
  uint32_t pc, lr, ctr, xer, msr, fpscr, srr0, srr1; std::array<uint32_t, 8> gqr; uint64_t ic;
};
inline void regs_save(const CPUContext &c, Regs &r) {
  r.gpr = c.gpr; r.fpr = c.fpr; r.ps1 = c.ps1; r.cr = c.cr; r.pc = c.pc; r.lr = c.lr; r.ctr = c.ctr; r.xer = c.xer;
  r.msr = c.msr; r.fpscr = c.fpscr; r.srr0 = c.srr0; r.srr1 = c.srr1; r.gqr = c.gqr; r.ic = c.inst_count;
}
inline void regs_load(CPUContext &c, const Regs &r) {
  c.gpr = r.gpr; c.fpr = r.fpr; c.ps1 = r.ps1; c.cr = r.cr; c.pc = r.pc; c.lr = r.lr; c.ctr = r.ctr; c.xer = r.xer;
  c.msr = r.msr; c.fpscr = r.fpscr; c.srr0 = r.srr0; c.srr1 = r.srr1; c.gqr = r.gqr; c.inst_count = r.ic;
}

// Ejecuta codigo invitado en el contexto de sonido hasta que vuelve a 0xFFFFFFFC (anidable).
// Bucle de despacho propio: todo el codigo de sonido esta en el DOL (0x80004000..0x80300000).
S2_NO_TSAN void snd_run_guest(CPUContext &ctx, uint32_t pc) {
  ctx.pc = pc;
  ctx.lr = 0xFFFFFFFC;
  for (;;) {
    const uint32_t t = ctx.pc;
    if (t == 0xFFFFFFFC) break;
    const uint32_t off = t - 0x80004000u;
    nwii_fn f = (!(off & 3) && off < 0x2FC000u && g_disp_table) ? g_disp_table[off >> 2] : nullptr;
    if (!f) {
      check_fail("pc fuera del codigo de sonido en la CPU de sonido", t, ctx.lr);
      std::printf("[S2] ERROR: la CPU de sonido salto a 0x%08x (lr 0x%08x): se abandona esa ejecucion\n", t, ctx.lr);
      break;
    }
    f(ctx);
  }
}

// Entrega una interrupcion (5 = AID, 7 = DSP) al manejador del juego, como un nivel de interrupcion:
// guarda todo el banco de registros, corre el manejador hasta el final (EE = 0 => S tomado) y lo restaura.
void snd_run_irq(CPUContext &ctx, int n) {
  const uint32_t handler = rd32(0x80003040 + n * 4);
  if (handler == 0 || handler == 0xFFFFFFFF) { check_fail("manejador nulo", n); return; }
  const uint64_t t0 = now_ns();
  Regs r;
  regs_save(ctx, r);
  uint32_t sp = (ctx.gpr[1] - 0x200) & ~0xFu;
  const uint32_t octx = sp - 0x2D0;  // OSContext de pega para el argumento `context`
  sp = (octx - 0x40) & ~0xFu;
  if (sp < snd_stack_end + 0x1000) { check_fail("pila de sonido casi agotada", sp, snd_stack_end); }
  wr32(sp, 0);
  ctx.gpr[1] = sp;
  ctx.gpr[3] = (uint32_t)n;
  ctx.gpr[4] = octx;
  ctx.msr &= ~0x8000u;
  snd_ee_sync(ctx);  // toma S
  ++in_irq;
  g_snd_in_irq.store(1, std::memory_order_relaxed);
  snd_run_guest(ctx, handler);
  g_snd_in_irq.store(0, std::memory_order_relaxed);
  if (g_axl_check) axlist_check("fin de interrupcion");
  --in_irq;
  regs_load(ctx, r);
  snd_ee_sync(ctx);  // lo suelta si el contexto interrumpido tenia EE = 1
  if (in_irq == 0 && !deferred_wakes.empty()) {
    for (size_t i = 0; i < deferred_wakes.size(); ++i) { add(st.req_wake); request(ctx, kReqWake, deferred_wakes[i]); }
    deferred_wakes.clear();
  }
  add(n == 5 ? st.irq_aid : st.irq_dsp);
  const uint64_t dt_irq = now_ns() - t0;
  add(st.irq_ns, dt_irq);
  if (n != 5) add(st_a_dsp_ns, dt_irq);
}

// Espera de la CPU de sonido a nivel de hilo (equivale a "el hilo duerme y corren otros"): mientras
// tanto se atienden los eventos y las interrupciones de sonido, con el candado S suelto.
template <class Pred> void snd_wait(CPUContext &ctx, Pred done, uint64_t max_us) {
  const uint32_t msr0 = ctx.msr;
  ctx.msr |= 0x8000;
  snd_ee_sync(ctx);
  const uint64_t t_end = max_us ? now_ns() + max_us * 1000 : UINT64_MAX;
  for (;;) {
    snd_pump(ctx);
    if (done()) break;
    if (snd_state.load(std::memory_order_relaxed) == kQuit) longjmp(snd_exit_jb, 2);
    if (now_ns() >= t_end) break;
    uint64_t due = next_due();
    if (max_us) due = std::min<uint64_t>(due, snd_now() + max_us * ctx.tb_freq / 1000000);
    snd_sleep_until(ctx, due);
  }
  ctx.msr = msr0;
  snd_ee_sync(ctx);
}

// Peticion sincrona a la CPU del juego.
uint32_t request(CPUContext &ctx, uint32_t op, uint32_t arg, uint32_t arg2) {
  // Sin S mientras se espera: la CPU del juego puede estar esperando S para entrar en una seccion.
  const uint32_t msr0 = ctx.msr;
  if (!(msr0 & 0x8000)) { ctx.msr |= 0x8000; snd_ee_sync(ctx); add(st.mtx_in_sec); }
  rq.op = op; rq.arg = arg; rq.arg2 = arg2; rq.result = 0;
  g_req.store(1, std::memory_order_release);
  const uint64_t t0 = now_ns();
  g_snd_wait_t0.store(t0, std::memory_order_relaxed);
  g_snd_wait.store(2, std::memory_order_relaxed);
  uint32_t spins = 0;
  for (;;) {
    if (g_req.load(std::memory_order_acquire) == 2) break;
    uint32_t p = 1;
    if (g_park.compare_exchange_strong(p, 2, std::memory_order_acquire, std::memory_order_relaxed)) {
      // el hilo del juego duerme en el bucle ocioso: la peticion se atiende aqui mismo
      if (game_can_service(*g_main)) { service(*g_main); add(st.req_self); }
      g_park.store(1, std::memory_order_release);
      if (g_req.load(std::memory_order_acquire) == 2) break;
    }
    if ((spins & 63) == 0 || spins > g_a_reqspin) g_main->poll_kick();
    cpu_relax();
    // Espera corta girando (el hilo del juego atiende en su siguiente sondeo: microsegundos); si no llega
    // es que esta parado en el anfitrion (carga de disco, GPU): se duerme cada vez mas para no quitarle
    // el nucleo al trabajador de render (misma prioridad o menor, mismo nucleo).
    if (++spins > g_a_reqspin) {
      const uint32_t k = spins - g_a_reqspin;
      add(st_a_req_sleep);
      std::this_thread::sleep_for(std::chrono::microseconds(k < 20 ? 20 : k < 60 ? 100 : 500));
    }
    if (snd_state.load(std::memory_order_relaxed) == kQuit) break;
  }
  g_req.store(0, std::memory_order_release);
  g_snd_wait.store(0, std::memory_order_relaxed);
  add(st.req_wait_ns, now_ns() - t0);
  add(st_a_req_spin, spins);
  if (!(msr0 & 0x8000)) { ctx.msr = msr0; snd_ee_sync(ctx); }
  return rq.result;
}

// Despertar a los hilos del juego que esperan en una cola. Dentro de un manejador de interrupcion se
// deja para su salida, como en la consola (el hilo despertado no corre hasta que la interrupcion
// termina) y para que el callback del hilo del juego no choque con el candado S del manejador.
void snd_wake(CPUContext &ctx, uint32_t q) {
  if (in_irq) {
    for (uint32_t d : deferred_wakes) if (d == q) return;
    deferred_wakes.push_back(q);
    return;
  }
  add(st.req_wake);
  request(ctx, kReqWake, q);
}

inline uint32_t snd_cur_thread() { return cur_fiber ? cur_fiber->thread : fibers[0].thread; }
int *mtx_depth(uint32_t m) {
  for (auto &e : snd_mtx) if (e.m == m) return &e.depth;
  return nullptr;
}
void snd_lock(CPUContext &ctx, uint32_t m) {
  if (int *d = mtx_depth(m)) { ++*d; return; }
  uint64_t nap = 100;  // us; crece mientras el mutex siga tomado (el juego lo tiene segundos al cargar)
  for (;;) {
    if (rd32(m + kMtxThread) == 0) {  // vistazo (sin sincronizar): solo decide si merece la pena pedirlo
      add(st.req_lock);
      if (request(ctx, kReqLock, m, snd_cur_thread())) break;
      add(st.req_lock_fail);
    }
    add(st.mtx_waits);
    snd_wait(ctx, [&] { return rd32(m + kMtxThread) == 0; }, nap);
    if (nap < 1600) nap *= 2;
  }
  snd_mtx.push_back({m, 1});
}
bool snd_trylock(CPUContext &ctx, uint32_t m) {
  if (int *d = mtx_depth(m)) { ++*d; return true; }
  add(st.req_lock);
  if (!request(ctx, kReqLock, m, snd_cur_thread())) { add(st.req_lock_fail); return false; }
  snd_mtx.push_back({m, 1});
  return true;
}
void snd_unlock(CPUContext &ctx, uint32_t m) {
  int *d = mtx_depth(m);
  if (!d) { check_fail("OSUnlockMutex en la CPU de sonido de un mutex que no tomo", m); return; }
  if (--*d > 0) return;
  for (size_t i = 0; i < snd_mtx.size(); ++i)
    if (snd_mtx[i].m == m) { snd_mtx.erase(snd_mtx.begin() + i); break; }
  if (snd_mtx.empty()) unboost(4);
  add(st.req_unlock);
  request(ctx, kReqUnlock, m, snd_cur_thread());
}

const char *fn_name(uint32_t fn) {
  switch (fn) {
    case 0x801B3150: return "OSWaitCond"; case 0x801B53A0: return "OSExitThread"; case 0x801B4EA0: return "SelectThread";
    case 0x801B50D0: return "__OSReschedule"; case 0x801AB5B0: return "OSSetPeriodicAlarm"; case 0x801CAB20: return "DVDReadPrio";
    default: return "?";
  }
}

// Ganchos de entrada (E2) en el contexto de sonido. true = resuelto aqui (la funcion invitada no corre).
S2_NO_TSAN bool snd_os(CPUContext &ctx, uint32_t fn, uint32_t lr) {
  add(st_a_os);
  switch (fn) {
    case F_OSLockMutex:
      if (lr == kRegLockLr || lr == kUnregLockLr) axl_note(ctx.gpr[30], snd_cur_thread(), lr, 1, snd_holds, (uint8_t)in_irq);
      snd_lock(ctx, ctx.gpr[3]);
      return true;
    case F_OSUnlockMutex: snd_unlock(ctx, ctx.gpr[3]); return true;
    case F_OSTryLockMutex: ctx.gpr[3] = snd_trylock(ctx, ctx.gpr[3]) ? 1 : 0; return true;
    case F_OSSendMessage: {
      const uint32_t mq = ctx.gpr[3];
      Fiber *f = fiber_of_queue(mq);
      if (!f) {
        // Cola de un hilo de la CPU del juego: se encola alli y se despierta al receptor.
        if (ctx.gpr[5] & 1) break;  // con bloqueo: no esperado
        add(st.req_send);
        const uint32_t ok = request(ctx, kReqSend, mq, ctx.gpr[4]);
        if (ok == 2) f = fiber_of_queue(mq);
        else {
          if (ok) snd_wake(ctx, mq + kMqRecvQueue);
          ctx.gpr[3] = ok;
          return true;
        }
      }
      if (f->n < (int)f->qcap) {
        f->msgs[(f->head + f->n) & 7] = ctx.gpr[4];
        ++f->n;
        ctx.gpr[3] = 1;
      } else {
        ctx.gpr[3] = 0;  // cola llena, como en la consola cuando el hilo va retrasado
        add(st.msg_drop);
      }
      return true;
    }
    case F_OSReceiveMessage: {
      Fiber *f = cur_fiber;
      if (!f || ctx.gpr[3] != f->queue || in_irq) break;
      if (f->n == 0) {
        if (!(ctx.gpr[5] & 1)) { ctx.gpr[3] = 0; return true; }
        if (lr != f->recv_ret) break;
        s2_longjmp(f->jb);  // la fibra se queda sin trabajo: vuelta al planificador de la CPU de sonido
      }
      const uint32_t m = f->msgs[f->head];
      f->head = (f->head + 1) & 7;
      --f->n;
      if (ctx.gpr[4]) wr32(ctx.gpr[4], m);
      ctx.gpr[3] = 1;
      return true;
    }
    case F_OSSleepThread:
      // Todos los que duermen en codigo de sonido vuelven a mirar su condicion en un bucle: una siesta
      // corta (atendiendo interrupciones) equivale a un despertar espurio.
      add(st.naps);
      snd_wait(ctx, [] { return false; }, 150);
      return true;
    case F_OSWakeupThread: snd_wake(ctx, ctx.gpr[3]); return true;
    case F_OSSetCurrentContext: return true;  // el "contexto actual" del SO es de la CPU del juego
    case F_OSGetCurrentThread: ctx.gpr[3] = snd_cur_thread(); return true;
    case F_OSYieldThread: return true;
    default: break;
  }
  // No deberia ejecutarse en la CPU de sonido: toca el planificador o al hilo actual.
  const uint64_t n = st.traps.fetch_add(1, std::memory_order_relaxed);
  if (n < 20) {
    std::printf("[S2] AVISO: la CPU de sonido entro en 0x%08x (%s) desde 0x%08x, r3=0x%08x: no interceptada (irq %d, fibra %s, pc 0x%08x, cadena:",
                fn, fn_name(fn), lr, ctx.gpr[3], in_irq, cur_fiber ? cur_fiber->name : "-", ctx.pc);
    uint32_t sp = ctx.gpr[1];
    for (int i = 0; i < 10 && sp >= 0x80000000u && sp < 0x81800000u; ++i) {
      std::printf(" 0x%08x", rd32(sp + 4));
      sp = rd32(sp);
    }
    std::printf(")\n");
  }
  return false;
}

// ---- toma y devolucion (hilo del juego) ---------------------------------------------------
bool fiber_ready(const Fiber &f) {
  const uint32_t th = f.thread, q = f.queue;
  if (g_main->mmu.read16(th + kThState) != 4) return false;       // WAITING
  if (rd32(th + kThQueue) != q + kMqRecvQueue) return false;      // dormido en el OSReceiveMessage de su bucle
  if (rd32(q + kMqUsed) != 0) return false;
  if (rd32(th + kThMtxHead) != 0) return false;
  return true;
}
// Prepara la pila de la fibra: la parte libre de la pila del hilo de verdad, por debajo de su marco dormido.
bool fiber_prepare(Fiber &f) {
  const uint32_t th = f.thread;
  const uint32_t sp_saved = rd32(th + 4);  // r1 guardado del hilo dormido
  const uint32_t end = rd32(th + kThStackEnd);
  if (end < 0x80000000u || sp_saved <= end || sp_saved - end < 0x2800 || rd32(end) != 0xDEADBABE || f.fsz < 16 || f.fsz > 0x400) {
    std::printf("[S2] %s: pila no valida (r1=0x%08x fin=0x%08x marco=%u): se queda en la CPU del juego\n", f.name, sp_saved, end, f.fsz);
    f.cap_ok = false;
    return false;
  }
  f.stack_end = end;
  f.sp = ((sp_saved - 0x200) & ~0xFu) - ((f.fsz + 0x1F) & ~0xFu);
  f.qcap = std::min<uint32_t>(std::max<uint32_t>(rd32(f.queue + 0x14), 1), 8);
  f.head = f.n = 0;
  std::printf("[S2] la CPU de sonido se queda con \"%s\": hilo=0x%08x cola=0x%08x (%u) pila 0x%08x..0x%08x (%u bytes libres)\n",
              f.name, th, f.queue, f.qcap, end, f.sp, f.sp - end);
  return true;
}
inline void work_bit(uint32_t bit, bool on) {
  const uint32_t w = g_game_work.load(std::memory_order_relaxed);
  g_game_work.store(on ? (w | bit) : (w & ~bit), std::memory_order_relaxed);
}
void update_work() {  // bit 1: hay alguna fibra capturada sin tomar
  bool want = false;
  for (int i = 0; i < kNumFibers; ++i)
    if (fibers[i].cap_ok && !fibers[i].hosted.load(std::memory_order_relaxed) && (i == 0 || g_fiber2)) want = true;
  work_bit(1, want);
}

void do_takeover(CPUContext &ctx) {
  CPUContext &s = *g_snd;
  Fiber &a = fibers[0];
  s.tb_freq = ctx.tb_freq; s.tb_start = ctx.tb_start;
  snd_slip = ctx.gov_slip;
  if (!fiber_prepare(a)) { update_work(); return; }
  if (!hw::s2_dsp_takeover()) return;  // el DSP no esta en reposo: se reintenta en otro sondeo
  s.gpr = ctx.gpr;  // r2 / r13; cada fibra pone los suyos al reanudarse
  s.fpr = ctx.fpr; s.ps1 = ctx.ps1; s.cr = ctx.cr; s.gqr = ctx.gqr; s.sprg = ctx.sprg;
  s.fpscr = ctx.fpscr; s.xer = ctx.xer; s.ctr = 0; s.hid2 = ctx.hid2;
  s.msr = ctx.msr | 0x8000;
  s.srr0 = s.srr1 = 0;
  s.gov_slip = ctx.gov_slip; s.gov_rate = CPUContext::kGovOne; s.gov_wall = ctx.gov_wall;
  s.gov_step_wall = ctx.gov_step_wall; s.gov_idle_wall = ctx.gov_wall; s.tb_cached = ctx.tb_cached;
  s.dec_irq_pending = false; s.poll_dec_due = false; s.in_callback = false; s.callback_depth = 0;
  s.dispatch_saved_ctx = 0; s.ext_resched_pending = false; s.inst_count = 0;
  s.gpr[1] = a.sp;  // nivel base (sin fibra): las interrupciones usan la pila del SoundThread
  s.pc = 0x80004000; s.lr = 0xFFFFFFFC;
  snd_stack_end = a.stack_end;
  in_irq = 0; snd_mtx.clear(); cur_fiber = nullptr;
  hb_req.store(false);
  a.hosted.store(true, std::memory_order_release);
  g_active.store(true, std::memory_order_release);
  update_work();
  add(st.takeovers);
  { std::lock_guard<std::mutex> lk(snd_mx); snd_go = true; snd_state.store(kRunning); }
  snd_cv.notify_all();
}

// El hilo del juego va a mandar un mensaje a la cola de un hilo que corre la CPU de sonido (apagado del
// sonido): la CPU de sonido termina lo que tenga, se aparca y los hilos invitados de verdad siguen.
void do_handback(CPUContext &ctx) {
  std::printf("[S2] devolucion del sonido al hilo del juego (mensaje 0x%08x a la cola 0x%08x)\n", ctx.gpr[4], ctx.gpr[3]);
  { std::lock_guard<std::mutex> lk(snd_mx); hb_req.store(true); }
  snd_cv.notify_all();
  uint32_t spins = 0;
  while (snd_state.load(std::memory_order_acquire) != kParked) {
    if (g_req.load(std::memory_order_relaxed) == 1) service(ctx);
    cpu_relax();
    if (++spins > 2000) std::this_thread::sleep_for(std::chrono::microseconds(50));
  }
  hw::s2_dsp_handback();
  for (Fiber &f : fibers) f.hosted.store(false, std::memory_order_release);
  g_active.store(false, std::memory_order_release);
  hb_req.store(false);
  update_work();  // las capturas siguen valiendo: los hilos de verdad siguen dormidos en su OSReceiveMessage
  add(st.handbacks);
}

// ---- el hilo anfitrion de la CPU de sonido ------------------------------------------------
// Una vuelta de una fibra: se reanuda en el retorno de su OSReceiveMessage con el mensaje `m` y corre
// hasta que vuelve a pedir un mensaje y no hay (longjmp desde el gancho).
int g_f2dbg = 0;  // NWII_S2_F2DBG (depuracion de la imagen negra): 1 = la fibra 2 no corre, 2 = corre con EE = 0
void run_fiber(CPUContext &ctx, Fiber &f, uint32_t m) {
  if (g_f2dbg == 1 && &f == &fibers[1]) { add(st.frames); return; }
  if (g_f2dbg == 3 && &f == &fibers[1]) {  // hipotesis: el juego necesita el cambio de hilo que provocaba este hilo
    g_main->queue_callback(F_OSYieldThread, 0, 0);
    g_main->poll_kick();
    wake_game_idle();
  }
  if (g_f2dbg == 5 && &f == &fibers[1]) {  // callback inocuo (OSGetCurrentThread) en la CPU del juego
    g_main->queue_callback(F_OSGetCurrentThread, 0, 0);
    g_main->poll_kick();
    wake_game_idle();
  }
  if (g_f2dbg == 4 && &f == &fibers[1]) {  // hipotesis DEC: el retorno de callback reinicia el decrementador
    g_game_work.fetch_or(4, std::memory_order_relaxed);
    g_main->poll_kick();
  }
  Regs base;
  regs_save(ctx, base);
  const uint64_t t0 = now_ns();
  ctx.gpr = f.gpr0;
  ctx.gpr[1] = f.sp;
  wr32(f.sp, 0);                         // fin de la cadena de marcos
  wr32(f.sp + f.fsz + 4, 0xFFFFFFFC);    // LR guardado de la funcion del hilo
  wr32(f.sp + f.out_ptr_off, m);         // el mensaje, donde lo dejaria OSReceiveMessage
  ctx.gpr[3] = 1;
  ctx.msr |= 0x8000;
  if (g_f2dbg == 2 && &f == &fibers[1]) { ctx.msr &= ~0x8000u; snd_ee_sync(ctx); }
  cur_fiber = &f;
  snd_stack_end = f.stack_end;
  if (setjmp(f.jb) == 0) {
    snd_run_guest(ctx, f.recv_ret);
    std::printf("[S2] AVISO: la funcion de \"%s\" termino en la CPU de sonido\n", f.name);
  }
  cur_fiber = nullptr;
  snd_stack_end = fibers[0].stack_end;
  ctx.is_running = true;
  regs_load(ctx, base);
  snd_ee_sync(ctx);
  add(st.frames);
  add(&f == &fibers[0] ? st.frame_ns : st.f2_ns, now_ns() - t0);
}

// [r35m4] salida del hilo de sonido al apagar el programa: si se sale (longjmp desde un manejador, EE = 0)
// con el candado S tomado, el hilo del juego giraba para siempre en game_acquire y el proceso no terminaba
// (visto en checks.sh con m4n: SIGTERM y el proceso seguia al 100 %).
void snd_quit_release() {
  if (snd_holds) { snd_holds = false; s_lock.store(0, std::memory_order_release); }
  if (g_req.load(std::memory_order_relaxed) == 1) g_req.store(0, std::memory_order_release);
  snd_exited.store(true);
}
void snd_thread_main() {
  t_snd = true;
#ifdef __SWITCH__
  {
    u64 allowed = 0;
    svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    const char *ce = std::getenv("NWII_S2_CORE");
    const int want = ce ? std::atoi(ce) : 2;
    int core = -1;
    for (int c : {want, 2, 0})
      if (c >= 0 && c < 3 && (allowed & (1ull << c)) && R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, c, 1u << c))) {
        core = c;
        break;
      }
    const char *pe = std::getenv("NWII_S2_PRIO");
    const int prio = pe ? (int)std::strtol(pe, nullptr, 0) : 0x2C;
    const Result prc = svcSetThreadPriority(CUR_THREAD_HANDLE, prio);
    g_prio_now.store(prio);
    std::printf("[NX] S2: CPU de sonido en el nucleo %d, prioridad 0x%X%s\n", core, prio, R_SUCCEEDED(prc) ? "" : " (fallo)");
    nx::note_thread(nx::kThreadS2);
  }
#endif
#ifndef __SWITCH__
  g_snd_pthread = pthread_self();
  pc_sched(5);
  g_prio_now.store(g_prio_lo);
#endif
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(snd_mx);
      snd_cv.wait(lk, [] { return snd_go || snd_state.load() == kQuit; });
      if (snd_state.load() == kQuit) { snd_quit_release(); return; }
      snd_go = false;
    }
    CPUContext &s = *g_snd;
    snd_holds = false;
    s.is_running = true;
    if (setjmp(snd_exit_jb) != 0) { snd_quit_release(); return; }  // apagado del programa
    // Planificador de la CPU de sonido: eventos e interrupciones; luego la fibra con mensajes.
    for (;;) {
      snd_pump(s);
      bool ran = false;
      for (Fiber &f : fibers)
        if (f.hosted.load(std::memory_order_acquire) && f.n > 0) {  // [r35m4] hosted antes que n (TSan)
          const uint32_t m = f.msgs[f.head];
          f.head = (f.head + 1) & 7;
          --f.n;
          run_fiber(s, f, m);
          ran = true;
          break;
        }
      if (ran) continue;
      if (snd_state.load(std::memory_order_relaxed) == kQuit) { snd_quit_release(); return; }
      if (hb_req.load(std::memory_order_relaxed) && snd_mtx.empty() && hw::s2_dsp_quiescent() && !hw::s2_dsp_irq()) break;
      s.note_idle();
      unboost(snd_holds ? 1u : snd_mtx.empty() ? 7u : 3u);  // r38c: al dia (y sin S / mutex): vuelve a la base
      snd_sleep_until(s, next_due());
    }
    if (snd_holds) { snd_holds = false; s_lock.store(0, std::memory_order_release); }
    snd_state.store(kParked, std::memory_order_release);
  }
}


// ==== r38c (frente C): vigilante de cuelgues ===============================================
// Hilo anfitrion de prioridad normal (0x2C, nucleo 0; duerme 1 s). Si ni el render (g_stat_frames) ni el juego
// (copias XFB) avanzan en ~5 s, escribe al registro normal unas pocas lineas [VIGIA] (~2-3 KB) y hace flush:
// estado del candado S, de la CPU de sonido (que espera y desde cuando), del candado del FIFO de GX y del DSP,
// % de CPU de cada hilo anfitrion en el intervalo, y los hilos invitados (estado, cola en que esperan, dueno del
// mutex). Repite cada 5 s mientras dure, como mucho 4 veces por parada y 8 en total. NWII_C_VIGIA_OFF=1 lo apaga.
}  // namespace (anonimo)
namespace {
CPUContext *g_wd_ctx = nullptr;
}  // namespace
#ifndef __SWITCH__
void pc_unsched_all();
#endif
namespace {
inline bool guest_ok(uint32_t a) { return (a >= 0x80000000u && a < 0x817FFFF0u) || (a >= 0x90000000u && a < 0x93FFFFF0u); }
inline uint32_t wd_rd32(uint32_t a) { return guest_ok(a) ? g_wd_ctx->mmu.read32(a) : 0xFFFFFFFFu; }
inline uint32_t wd_rd16(uint32_t a) { return guest_ok(a) ? g_wd_ctx->mmu.read16(a) : 0xFFFFu; }
const char *wait_name(uint32_t w) {
  switch (w) { case 0: return "nada"; case 1: return "candado S"; case 2: return "peticion al juego"; case 3: return "candado DSP"; default: return "?"; }
}
void vigia_dump(int nth, double stalled_s) {
  const uint64_t now = now_ns();
  std::printf("[VIGIA] llamadas por puntero nulo: %llu (ultimo sitio 0x%08x)\n", (unsigned long long)g_null_calls.load(),
              g_null_last_site.load());
  if (g_enabled && nth <= 2) { std::printf("[VIGIA] ultimas altas/bajas en la lista AX de SoundThread:\n"); axl_ring_dump(); }
  std::printf("[VIGIA] #%d sin progreso %.1f s (render fot %llu, copias XFB %llu)\n", nth, stalled_s,
              (unsigned long long)c38_frames_render(), (unsigned long long)c38_frames_game());
#ifdef __SWITCH__
  {  // % de CPU de cada hilo en el ultimo segundo
    static const char *const nm[] = {"main", "cpu", "ax", "aud", "gxw", "snd"};
    static uint64_t last[6] = {}, last_t = 0;
    const uint64_t t = armGetSystemTick();
    char b[200]; int k = 0;
    for (int i = 0; i < 6; ++i) {
      const Handle h = nx::thread_handle(i);
      u64 ticks = 0;
      if (!h || R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, (u64)-1))) continue;
      if (last_t) k += std::snprintf(b + k, sizeof b - k, " %s %.0f%%", nm[i], 100.0 * (double)(ticks - last[i]) / (double)(t - last_t));
      last[i] = ticks;
    }
    last_t = t;
    std::printf("[VIGIA] cpu%%:%s\n", b);
  }
#endif
  if (g_wd_ctx) {
    CPUContext &c = *g_wd_ctx;
    std::printf("[VIGIA] juego: pc 0x%08x lr 0x%08x msr 0x%x hilo 0x%08x en_callback %d (prof %d) | espera S desde %.1f ms\n", c.pc, c.lr,
                c.msr, wd_rd32(kCurThread), (int)c.in_callback, c.callback_depth,
                g_game_wait_t0.load() ? (now - g_game_wait_t0.load()) / 1e6 : 0.0);
  }
  if (g_enabled) {
    const uint64_t sw = g_snd_wait_t0.load();
    std::printf("[VIGIA] S2: activo %d estado %u | S=%u (1 juego, 2 sonido) juego %s dueno 0x%08x tomado en pc 0x%08x lr 0x%08x | sonido S %d irq %d "
                "fibra %s | espera %s desde %.1f ms | peticion %u (op %u arg 0x%08x) park %u | max espera S: juego %llu us, sonido %llu us | siestas %llu\n",
                (int)g_active.load(), snd_state.load(), s_lock.load(), gs.holds == kHoldIrq ? "irq" : gs.holds == kHoldSection ? "secc" : "-", gs.owner,
                gs_acq_site, gs_acq_lr, (int)snd_holds, in_irq, cur_fiber ? cur_fiber->name : "-", wait_name(g_snd_wait.load()),
                g_snd_wait.load() && sw ? (now - sw) / 1e6 : 0.0, g_req.load(), rq.op, rq.arg, g_park.load(),
                (unsigned long long)st_game_wait_max_us.load(), (unsigned long long)st_snd_wait_max_us.load(),
                (unsigned long long)st_backoff_sleeps.load());
    if (g_snd) std::printf("[VIGIA] S2 ctx: pc 0x%08x lr 0x%08x msr 0x%x | fibras: %s hosted %d n %d, %s hosted %d n %d | mutex del sonido %zu\n",
                           g_snd->pc, g_snd->lr, g_snd->msr, fibers[0].name, (int)fibers[0].hosted.load(), fibers[0].n, fibers[1].name,
                           (int)fibers[1].hosted.load(), fibers[1].n, snd_mtx.size());
  }
  {
    char b[256];
    c38_gx_state(b, sizeof b);
    std::printf("[VIGIA] gx: %s | dsp: candado %d\n", b, (int)hw::c38_dsp_lock_busy());
  }
  // hilos invitados (0x800000DC, enlace +0x2FC): estado, prioridad, cola, pc/lr guardados
  if (g_wd_ctx) {
    uint32_t t = wd_rd32(0x800000DC);
    std::printf("[VIGIA] hilos invitados (actual 0x%08x):", wd_rd32(kCurThread));
    for (int i = 0; i < 20 && guest_ok(t); ++i) {
      const uint32_t q = wd_rd32(t + kThQueue);
      std::printf(" | 0x%08x st %u pri %u q 0x%08x", t, wd_rd16(t + kThState), wd_rd32(t + kThPrio), q);
      if (guest_ok(q)) std::printf(" (mutex? dueno 0x%08x)", wd_rd32(q + kMtxThread));
      std::printf(" srr0 0x%08x lr 0x%08x", wd_rd32(t + 0x198), wd_rd32(t + 0x84));
      t = wd_rd32(t + 0x2FC);
    }
    std::printf("\n");
  }
  c38_log_flush();
}
void vigia_main() {
#ifdef __SWITCH__
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x2C);
#endif
  uint64_t lr = c38_frames_render(), lg = c38_frames_game(), t_prog = now_ns();
  int episode = 0, total = 0;
  for (;;) {
    sleep_us(1000000);
    const uint64_t r = c38_frames_render(), g = c38_frames_game(), now = now_ns();
    {  // r38c: rafaga de llamadas por puntero nulo (el cuelgue de la lista AX dejaba ~100 k/s): alarma aunque haya fotogramas
      static uint64_t last_nc = 0;
      const uint64_t nc = g_null_calls.load(std::memory_order_relaxed);
      if (nc - last_nc > 1000 && total < 8) { ++total; std::printf("[VIGIA] ALARMA: %llu llamadas por puntero nulo en 1 s\n", (unsigned long long)(nc - last_nc)); vigia_dump(total, 0.0); }
      last_nc = nc;
    }
    if (r != lr || g != lg) { lr = r; lg = g; t_prog = now; episode = 0; continue; }
    const double st = (now - t_prog) / 1e9;
    if (st >= 5.0 * (episode + 1) && episode < 4 && total < 8) {
      ++episode; ++total; vigia_dump(total, st);
#ifndef __SWITCH__
      if (episode == 2) pc_unsched_all();  // PC con NWII_C_PCSCHED: romper un bloqueo por prioridades y ver si sigue
#endif
    }
  }
}
}  // namespace
void vigia_start(CPUContext *main_ctx) {
  const char *off = NWII_ENV("NWII_C_VIGIA_OFF");
  if (off && *off) return;
  g_wd_ctx = main_ctx;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  pthread_t th;
  pthread_create(&th, &attr, [](void *) -> void * { vigia_main(); return nullptr; }, nullptr);
  pthread_detach(th);
  std::printf("[VIGIA] vigilante de cuelgues activo (5 s sin fotogramas -> volcado al registro)\n");
}
namespace {
}  // namespace

bool on_snd_thread() { return t_snd; }
}  // namespace s2
void c38_log_flush() {
#ifdef __SWITCH__
  nx::c38_sink_flush();
#else
  std::fflush(stdout);
#endif
}
namespace s2 {
void spin_backoff(uint32_t &spins) { backoff(spins); }
#ifdef __SWITCH__
void pc_sched(int) {}
bool pc_set_prio(int) { return false; }
#else
}  // namespace s2
}  // namespace nwii::runtime
#include <sched.h>
namespace nwii::runtime {
namespace s2 {
static bool pcs_on() { static const bool on = [] { const char *e = std::getenv("NWII_C_PCSCHED"); return e && *e && *e != '0'; }(); return on; }
static std::mutex pcs_mx;
static std::vector<pthread_t> pcs_threads;
// Valvula de seguridad (vigia): todos los hilos fijados vuelven a SCHED_OTHER (sin prioridades estrictas).
void pc_unsched_all() {
  std::lock_guard<std::mutex> lk(pcs_mx);
  sched_param sp{};
  for (pthread_t t : pcs_threads) pthread_setschedparam(t, SCHED_OTHER, &sp);
  std::printf("[C38] PCSCHED deshecho: %zu hilos vuelven a SCHED_OTHER\n", pcs_threads.size());
}
bool pc_set_prio(int hp) {
  if (!pcs_on()) return false;
  sched_param sp{};
  sp.sched_priority = std::max(1, 0x3C - hp);  // 0x2C -> 16, 0x2D -> 15, 0x3B -> 1
  return pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) == 0;
}
void pc_sched(int slot) {
  if (!pcs_on()) return;
  // nucleos de la Switch 0..2 -> CPUs del PC (NWII_C_PCSCHED_CPUS, por defecto 2,3,4)
  int cpus[3] = {2, 3, 4};
  if (const char *e = std::getenv("NWII_C_PCSCHED_CPUS")) std::sscanf(e, "%d,%d,%d", &cpus[0], &cpus[1], &cpus[2]);
  auto envi = [](const char *n, int d) { const char *e = std::getenv(n); return e ? (int)std::strtol(e, nullptr, 0) : d; };
  int core = 0, prio = 0x2C;
  switch (slot) {
    case 0: core = 0; prio = 0x2C; break;
    case 1: core = 1; prio = 0x3B; break;
    case 4: core = envi("NWII_RENDER_WORKER_CORE", 2); prio = envi("NWII_RENDER_WORKER_PRIO", 0x2D); break;
    case 5: core = envi("NWII_S2_CORE", 2); prio = envi("NWII_S2_PRIO", 0x2C); break;
    default: return;
  }
  cpu_set_t cs;
  CPU_ZERO(&cs);
  CPU_SET(cpus[core % 3], &cs);
  const int a = pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
  const bool p = pc_set_prio(prio);
  { std::lock_guard<std::mutex> lk(pcs_mx); pcs_threads.push_back(pthread_self()); }
  std::printf("[C38] PCSCHED: hilo %d -> nucleo %d (CPU %d%s), prioridad 0x%X%s\n", slot, core, cpus[core % 3], a ? " FALLO" : "", prio,
              p ? " (SCHED_FIFO)" : " FALLO");
}
#endif
CPUContext *snd_ctx() { return g_snd; }

uint64_t snd_now() { return g_snd->wall_timebase_now() - snd_slip; }
uint64_t snd_schedule_at(uint64_t due, Cb cb) {
  const uint64_t id = ev_next_id++;
  evs.push_back({due, id, std::move(cb)});
  return id;
}
bool snd_cancel(uint64_t id, uint64_t *due) {
  for (size_t i = 0; i < evs.size(); ++i)
    if (evs[i].id == id) {
      if (due) *due = evs[i].due;
      evs.erase(evs.begin() + i);
      return true;
    }
  return false;
}

bool snd_poll(CPUContext &ctx) {
  if (ctx.pc == 0xFFFFFFFC) {  // el codigo invitado lanzado por snd_run_guest ha vuelto
    ctx.is_running = false;
    return false;
  }
  if (g_check && !t_snd) check_fail("contexto de sonido ejecutado por otro hilo", ctx.pc);
  static const long s_irq_stress0 = [] { const char *e = NWII_ENV("NWII_C_STRESS_IRQ_US"); return e ? std::atol(e) : 0L; }();
  // r38c: con estres, cada vuelta del bucle de la lista AX (0x8027B91C-0x8027B938) pasa por aqui y se entretiene
  const bool in_axloop = s_irq_stress0 > 0 && in_irq && ctx.pc - 0x8027B91Cu <= 0x1Cu;
  ctx.poll_left.store(in_axloop ? 1 : 64, std::memory_order_relaxed);
  ctx.note_idle();
  add(st_a_polls);
  if (snd_state.load(std::memory_order_relaxed) == kQuit) longjmp(snd_exit_jb, 2);
  // r38c NWII_C_STRESS_IRQ_US (solo pruebas): la CPU de sonido se entretiene dentro de sus interrupciones (agranda la
  // ventana en que recorre la lista AX).
  static const long s_irq_stress = [] { const char *e = NWII_ENV("NWII_C_STRESS_IRQ_US"); return e ? std::atol(e) : 0L; }();
  if (__builtin_expect(s_irq_stress > 0, 0) && in_axloop) {
    const uint64_t until = now_ns() + (uint64_t)s_irq_stress * 1000;
    while (now_ns() < until) cpu_relax();
  }
  snd_pump(ctx);
  return false;
}

void game_poll_slow(CPUContext &ctx) {
  if (g_game_work.load(std::memory_order_relaxed) & 4) {
    g_game_work.fetch_and(~4u, std::memory_order_relaxed);
    ctx.dec_written_tb = ctx.read_timebase();
  }
  if (gs.holds || gs.list_owner) game_rules(ctx, false, false);
  if (g_req.load(std::memory_order_relaxed) == 1 && game_can_service(ctx)) service(ctx);
  if ((g_game_work.load(std::memory_order_relaxed) & 1) && game_can_service(ctx) && !gs.holds) {
    if (!g_active.load(std::memory_order_relaxed)) {
      static const bool notake = NWII_ENV("NWII_S2_NOTAKE") != nullptr;  // depuracion: ganchos si, CPU de sonido no
      if (!notake && fibers[0].cap_ok && snd_state.load(std::memory_order_relaxed) == kParked && fiber_ready(fibers[0])) do_takeover(ctx);
    } else if (!hb_req.load(std::memory_order_relaxed)) {
      for (int i = 1; i < kNumFibers; ++i) {
        Fiber &f = fibers[i];
        if (g_fiber2 && f.cap_ok && !f.hosted.load(std::memory_order_relaxed) && fiber_ready(f)) {
          if (fiber_prepare(f)) f.hosted.store(true, std::memory_order_release);
          update_work();
        }
      }
    }
  }
}

void game_irq_enter(CPUContext &ctx) {
  if (!gs.holds) game_acquire(ctx, kHoldIrq, rd32(kCurThread), true);
  // r38c NWII_C_STRESS_SFIFO=1 (solo pruebas): con S tomado, el juego toca el candado del FIFO de GX en cada
  // interrupcion (como el vaciado por edad de Z22 dentro del manejador de alarmas): agranda la ventana del cuelgue.
  static const bool s_stress = [] { const char *e = NWII_ENV("NWII_C_STRESS_SFIFO"); return e && *e && *e != '0'; }();
  if (__builtin_expect(s_stress, 0)) c38_stress_fifo_touch();
}
void game_sync(CPUContext &ctx) { game_rules(ctx, false, false); }

// r38c: SoundThread::Register/Unregister en el hilo del juego (ver kAxListHead). true = OSLockMutex resuelto aqui.
bool game_axlist(CPUContext &ctx, uint32_t fn, uint32_t lr) {
  const uint32_t cur = rd32(kCurThread);
  if (fn == F_OSUnlockMutex) {  // la lista ya esta cambiada: se suelta S y el SDK suelta el mutex como siempre
    if (g_axl_mutex && rd32(ctx.gpr[3] + kMtxCount) > 1) return false;  // desbloqueo interior (mutex recursivo)
    if (gs.list_owner == cur) {
      const uint64_t h = now_ns() - g_axl_t_lock;
      add(st_axl_hold_ns, h);
      note_max(st_axl_hold_max_us, h / 1000);
      gs.list_owner = 0;
      if (gs.holds == kHoldList) game_release();
    }
    return false;
  }
  add(st_axl_calls);
  if (g_snd_in_irq.load(std::memory_order_relaxed)) add(st_axl_overlap);
  axl_note(ctx.gpr[30], cur, lr, 0, gs.holds != 0, (uint8_t)g_snd_in_irq.load(std::memory_order_relaxed));  // la carrera: la CPU de sonido esta en su interrupcion
  if (!g_axl_fix || gs.holds || gs.list_owner) return false;
  const uint32_t m = ctx.gpr[3];
  const uint64_t t0 = now_ns();
  for (uint32_t spins = 0;; ++spins) {
    const uint32_t owner = rd32(m + kMtxThread);
    if (owner == 0 || owner == cur) {
      // Primero S (la CPU de sonido nunca espera al juego con S tomado: request() lo suelta), luego el mutex en
      // nativo, como OSLockMutex con el mutex libre (nadie mas toca el mutex: las peticiones del sonido las atiende
      // este mismo hilo).
      game_acquire(ctx, kHoldList, cur, true);
      if (rd32(m + kMtxThread) == 0 || rd32(m + kMtxThread) == cur) {
        do_trylock(m, cur);
        gs.list_owner = cur;
        g_axl_t_lock = now_ns();
        add(st_axl_wait_ns, now_ns() - t0);
        return true;
      }
      game_release();
      continue;
    }
    bool snd_owner = false;
    for (Fiber &f : fibers) if (owner == f.thread && f.hosted.load(std::memory_order_relaxed)) snd_owner = true;
    if (!snd_owner || spins > 2000000) {
      // Lo tiene otro hilo del juego: el SDK lo espera durmiendo como siempre (la alta/baja va entonces sin S; contado).
      add(st_axl_fallback);
      return false;
    }
    // Lo tiene la CPU de sonido (FrameProcess): se atienden sus peticiones (el desbloqueo es una) hasta que lo suelte.
    if (g_req.load(std::memory_order_relaxed) == 1 && game_can_service(ctx)) service(ctx);
    cpu_relax();
  }
}



void game_idle_sleep(uint64_t us) {
  // El hilo del juego duerme en el bucle ocioso del SO invitado (EE = 1, sin hilo): mientras tanto la
  // CPU de sonido puede atender ella misma sus peticiones (g_park), y lo despierta si encola un callback.
  g_park.store(1, std::memory_order_release);
  {
    std::unique_lock<std::mutex> lk(idle_mx);
    if (!idle_wake) idle_cv.wait_for(lk, std::chrono::microseconds(us));
    idle_wake = false;
  }
  for (;;) {
    uint32_t p = 1;
    if (g_park.compare_exchange_weak(p, 0, std::memory_order_acquire, std::memory_order_relaxed)) break;
    cpu_relax();
  }
}

void init(CPUContext *main_ctx) {
  { const char *e = NWII_ENV("NWII_C_SPIN_OFF"); g_spin_old = e && *e; }
  { const char *e = NWII_ENV("NWII_A_NOSPIN"); g_a_nospin = e && *e && *e != '0'; }  // r43a
  { const char *e = NWII_ENV("NWII_A_REQSPIN"); if (e && *e) g_a_reqspin = (uint32_t)std::max(1L, std::atol(e)); }
  { const char *e = NWII_ENV("NWII_C_AXLIST_OFF"); g_axl_fix = !(e && *e); }      // r38c
  { const char *e = NWII_ENV("NWII_C_AXLIST_CHECK"); g_axl_check = e && *e && *e != '0'; }
  { const char *e = NWII_ENV("NWII_C_AXLIST_MUTEX"); g_axl_mutex = g_axl_fix && e && *e && *e != '0'; }
  {  // r38c: prioridad dinamica (sin NWII_C_PRIO_HI: nada)
    const char *pl = NWII_ENV("NWII_S2_PRIO"), *ph = NWII_ENV("NWII_C_PRIO_HI"), *lt = NWII_ENV("NWII_C_LATE_US");
    g_prio_lo = pl ? (int)std::strtol(pl, nullptr, 0) : 0x2C;
    g_prio_hi = ph ? (int)std::strtol(ph, nullptr, 0) : 0;
    if (g_prio_hi >= g_prio_lo) g_prio_hi = 0;  // solo tiene sentido si sube
    if (lt) g_late_boost_us = std::strtoull(lt, nullptr, 0);
  }
  vigia_start(main_ctx);  // r38c
  // [r35m4 m5] S2 ENCENDIDO por defecto. Apagado: NWII_S2_OFF=1 (llega desde debug.toml; apply_env borra los
  // "0"/"off") o NWII_S2=0 explicito (PC).
  const char *e = NWII_ENV("NWII_S2");
  if (NWII_ENV("NWII_S2_OFF") || (e && *e == '0')) return;
  g_main = main_ctx;
  // Solo SMNE01 (USA): las direcciones estan fijadas a ese DOL.
  if (rd32(0x8027B960) != 0x9421FFE0u || rd32(0x801B1140) != 0x7C6000A6u || rd32(0x801B2E60) != 0x9421FFE0u) {
    std::printf("[S2] el DOL no es el esperado (SMNE01): NWII_S2 se ignora\n");
    return;
  }
  const char *c = NWII_ENV("NWII_S2_CHECK");
  g_check = c && *c && *c != '0';
  g_log = NWII_ENV("NWII_S2_LOG") != nullptr;
  { const char *f2 = NWII_ENV("NWII_S2_F2"); g_fiber2 = !(f2 && *f2 == '0'); }
  { const char *d = NWII_ENV("NWII_S2_F2DBG"); g_f2dbg = d ? std::atoi(d) : 0; }
  g_fm_alias_next = true;
  g_snd = new CPUContext();
  g_fm_alias_next = false;
  g_snd->mmu.mem1 = main_ctx->mmu.mem1;
  g_snd->mmu.mem2 = main_ctx->mmu.mem2;
  g_snd->mmu.lc = main_ctx->mmu.lc;
  g_snd->is_running = true;
  g_enabled = true;
  nwii_s2_on = true;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
  pthread_t th;
  pthread_create(&th, &attr, [](void *) -> void * { snd_thread_main(); return nullptr; }, nullptr);
  pthread_detach(th);
  std::printf("[S2] sonido en otro nucleo ACTIVADO (NWII_S2=1%s)%s\n", g_check ? ", NWII_S2_CHECK" : "",
              g_spin_old ? " [r38c: esperas como m5, NWII_C_SPIN_OFF]" : " [r38c: esperas que duermen]");
}

void shutdown() {
  if (!g_enabled) return;
  { std::lock_guard<std::mutex> lk(snd_mx); snd_state.store(kQuit); }
  snd_cv.notify_all();
  for (int i = 0; i < 100 && !snd_exited.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

void stats_line(char *buf, int n, double dt) {
  if (n <= 0) return;
  buf[0] = 0;
  if (!g_enabled) return;
  if (g_f2dbg >= 1 && g_main) {  // depuracion: lista de hilos invitados (0x800000DC, enlace activo +0x2FC)
    static int calls = 0;
    std::printf("[S2] actual(muestras):");
    for (CurSample &c : g_cur_hist) if (c.n) { std::printf(" 0x%08x=%u", c.th, c.n); c.n = 0; c.th = 0; }
    std::printf("\n");
    if ((++calls % 5) == 3) {
      uint32_t t = rd32(0x800000DC);
      std::printf("[S2] hilos: actual=0x%08x", rd32(kCurThread));
      for (int i = 0; i < 24 && t >= 0x80000000u && t < 0x81800000u; ++i) {
        std::printf(" | 0x%08x st=%u pri=%u base=%u q=0x%08x sus=%d lr:", t, g_main->mmu.read16(t + kThState), rd32(t + kThPrio),
                    rd32(t + kThBase), rd32(t + kThQueue), (int)rd32(t + 0x2CC));
        uint32_t sp = rd32(t + 4);
        for (int k = 0; k < 6 && sp >= 0x80000000u && sp < 0x81800000u; ++k) { sp = rd32(sp); if (sp < 0x80000000u || sp >= 0x81800000u) break; std::printf(" %08x", rd32(sp + 4)); }
        t = rd32(t + 0x2FC);
      }
      std::printf("\n");
    }
  }
  static uint64_t p[20] = {0};
  auto d = [&](int i, const std::atomic<uint64_t> &c) { const uint64_t v = c.load(std::memory_order_relaxed), r = v - p[i]; p[i] = v; return r; };
  const double k = dt > 0 ? 1.0 / dt : 0.0;
  const uint64_t aid = d(0, st.irq_aid), dsp = d(1, st.irq_dsp), fr = d(2, st.frames), irqns = d(3, st.irq_ns), frns = d(4, st.frame_ns);
  const uint64_t lk = d(5, st.req_lock), lf = d(6, st.req_lock_fail), self = d(7, st.req_self), rw = d(8, st.req_wait_ns);
  const uint64_t gsec = d(9, st.g_sec), girq = d(10, st.g_irq), gw = d(11, st.g_wait_ns), gwn = d(12, st.g_wait_n);
  const uint64_t sw = d(13, st.s_wait_ns), mw = d(14, st.mtx_waits), wk = d(15, st.wakes_cb), f2ns = d(16, st.f2_ns), snd = d(17, st.req_send);
  const uint64_t late = st.late_max_us.exchange(0, std::memory_order_relaxed);
  std::snprintf(buf, n,
                "[S2] %s: AID %.0f/s DSP %.0f/s tramas %.0f/s | sonido: irq %.1f ms/s, hilo %.1f + %.1f ms/s, espera S %.2f ms/s, retraso max %llu us | "
                "mutex %.0f/s (fallo %.0f/s, espera %.0f/s, sin juego %.0f/s, %.2f ms/s) envios %.0f/s despertar %.0f/s | juego: S secc %.0f/s irq %.0f/s, "
                "espera %.2f ms/s en %.0f/s | total: tomas %llu dev %llu perdidos %llu trampas %llu asserts %llu stash %llu",
                g_active.load() ? (fibers[1].hosted.load() ? "ACTIVO(2)" : "ACTIVO(1)") : "en espera", aid * k, dsp * k, fr * k, irqns * k / 1e6,
                frns * k / 1e6, f2ns * k / 1e6, sw * k / 1e6,
                (unsigned long long)late, lk * k, lf * k, mw * k, self * k, rw * k / 1e6, snd * k, wk * k, gsec * k, girq * k, gw * k / 1e6, gwn * k,
                (unsigned long long)st.takeovers.load(), (unsigned long long)st.handbacks.load(), (unsigned long long)st.msg_drop.load(),
                (unsigned long long)st.traps.load(), (unsigned long long)st.checks_bad.load(), (unsigned long long)st.g_stash.load());
  // r38c: segunda linea [C38] (subidas de prioridad, recuperacion de pasadas del DMA, esperas maximas, siestas)
  static uint64_t q[10] = {0};
  auto dq = [&](int i, uint64_t v) { const uint64_t r = v - q[i]; q[i] = v; return r; };
  const uint64_t b0 = dq(0, st_boost_n[0].load()), b1 = dq(1, st_boost_n[1].load()), b2 = dq(2, st_boost_n[2].load());
  const uint64_t bns = dq(3, st_boost_ns.load()), sl = dq(4, st_backoff_sleeps.load());
  uint64_t cu = 0, rs = 0;
  hw::c38_dsp_catchup_stats(&cu, &rs);
  const uint64_t dcu = dq(5, cu), drs = dq(6, rs);
  static uint64_t axw_last = 0;
  const uint64_t axw_now = st_axl_wait_ns.load(), axw_ns = axw_now - axw_last;
  axw_last = axw_now;
  const size_t used = std::strlen(buf);
  std::snprintf(buf + used, n - (int)used,
                "\n[C38] prio base 0x%X alta 0x%X | subidas: tarde %.0f/s juego-S %.0f/s juego-mutex %.0f/s, en alta %.1f ms/s | DMA: "
                "pasadas recuperadas %.0f/s, reinicios %.0f/s | max espera S: juego %llu us, sonido %llu us | siestas %.0f/s | "
                "lista AX%s: altas/bajas %.0f/s, con la CPU de sonido en interrupcion %llu, sin S %llu, espera %.3f ms/s, rotas %llu, "
                "S retenido %.2f ms/s (max %llu us)",
                g_prio_lo, g_prio_hi, b0 * k, b1 * k, b2 * k, bns * k / 1e6, dcu * k, drs * k,
                (unsigned long long)st_game_wait_max_us.exchange(0), (unsigned long long)st_snd_wait_max_us.exchange(0), sl * k,
                g_axl_fix ? (g_axl_mutex ? " (regla del mutex)" : "") : " (SIN arreglo)", dq(7, st_axl_calls.load()) * k, (unsigned long long)st_axl_overlap.load(),
                (unsigned long long)st_axl_fallback.load(), axw_ns * k / 1e6, (unsigned long long)st_axl_bad.load(),
                dq(8, st_axl_hold_ns.load()) * k / 1e6, (unsigned long long)st_axl_hold_max_us.exchange(0));
  // r43a: tercera linea [A43]
  static uint64_t a[8] = {0};
  auto da = [&](int i, uint64_t v) { const uint64_t r = v - a[i]; a[i] = v; return r; };
  const uint64_t cvw = da(0, st_a_cvwait.load()), yl = da(1, st_a_yield.load()), np = da(2, st_a_nap.load());
  const uint64_t pl = da(3, st_a_polls.load()), dns = da(4, st_a_dsp_ns.load()), rsp = da(5, st_a_req_spin.load());
  const uint64_t rsl = da(6, st_a_req_sleep.load()), nreq = lk + d(18, st.req_unlock) + d(19, st.req_wake);
  const uint64_t sacq = da(7, st.s_acq.load()), nos = dq(9, st_a_os.load());
  const size_t used2 = std::strlen(buf);
  std::snprintf(buf + used2, n - (int)used2,
                "\n[A43] sonido: esperas cv %.0f/s, cesiones %.0f/s, siestas cortas %.0f/s | sondeos %.0f/s | irq DSP %.1f ms/s (AID %.1f) | "
                "peticiones %.0f/s, giro %.0f vueltas/pet, siestas %.0f/s | S tomado %.0f/s, ganchos SO %.0f/s | nospin %d reqspin %u",
                cvw * k, yl * k, np * k, pl * k, dns * k / 1e6, (irqns - dns) * k / 1e6, nreq * k,
                nreq ? (double)rsp / nreq : 0.0, rsl * k, sacq * k, nos * k, (int)g_a_nospin, g_a_reqspin);
}

}  // namespace s2
}  // namespace nwii::runtime

// ---- ganchos del codigo generado (pasada tools/apply_s2.py) ----------------------------------
using nwii::runtime::CPUContext;
namespace s2 = nwii::runtime::s2;
extern "C" {
bool nwii_s2_on = false;

// E1: tras cada escritura de MSR (mtmsr / rfi). lr = direccion de retorno del que llamo a la funcion
// (para las copias pegadas de OSDisableInterrupts, la del sitio de la llamada original).
void nwii_s2_ee(CPUContext &ctx, uint32_t ee_was, uint32_t lr, uint32_t site) {
  using namespace nwii::runtime::s2;
  if (&ctx != g_main) {
    if (g_check && !t_snd) check_fail("E1: contexto de sonido en otro hilo", site);
    snd_ee_sync(ctx);
    return;
  }
  if (g_check && t_snd) check_fail("E1: contexto del juego en el hilo de sonido", site);
  const bool ee = ctx.msr & 0x8000;
  if (!gs.holds && (ee || (gs.nstash == 0 && !(site == 0x801B1148u && snd_class(lr))))) return;  // lo comun
  game_rules(ctx, site == 0x801B1148u && snd_class(lr), ee_was != 0);
}

// E2: entrada de funciones del SO. true = ya resuelta (solo en el contexto de sonido).
S2_NO_TSAN bool nwii_s2_os(CPUContext &ctx, uint32_t fn, uint32_t lr) {
  using namespace nwii::runtime::s2;
  if (&ctx != g_main) return snd_os(ctx, fn, lr);
  if (fn == F_OSReceiveMessage) {
    for (Fiber &f : fibers)
      if (lr == f.recv_ret && !f.hosted.load(std::memory_order_relaxed)) {
        // Este hilo invitado va a dormirse en su cola: desde aqui la CPU de sonido puede quedarselo.
        const uint32_t r1 = ctx.gpr[1], back = rd32(r1);
        if (!f.cap_ok) std::printf("[S2] visto \"%s\": hilo=0x%08x cola=0x%08x\n", f.name, rd32(kCurThread), ctx.gpr[3]);
        f.queue = ctx.gpr[3];
        f.thread = rd32(kCurThread);
        f.gpr0 = ctx.gpr;
        f.out_ptr_off = ctx.gpr[4] - r1;
        f.fsz = back - r1;
        f.cap_ok = f.out_ptr_off < 0x400;
        update_work();
      }
  } else if (fn == F_OSSendMessage) {
    if (g_active.load(std::memory_order_relaxed) && fiber_of_queue(ctx.gpr[3])) do_handback(ctx);
  } else if ((fn == F_OSLockMutex || fn == F_OSUnlockMutex) && g_active.load(std::memory_order_relaxed) &&
             ((lr == kRegLockLr || lr == kUnregLockLr || lr == kRegUnlockLr || lr == kUnregUnlockLr) || (g_axl_mutex && ctx.gpr[3] == kSndMutex))) {
    if (game_axlist(ctx, fn, lr)) return true;
  }
  if (fn == F_OSLockMutex && g_prio_hi && g_active.load(std::memory_order_relaxed)) {
    // r38c: herencia: si el mutex es de un hilo que corre la CPU de sonido, el juego va a dormir esperandolo
    const uint32_t owner = rd32(ctx.gpr[3] + kMtxThread);
    if (owner)
      for (Fiber &f : fibers)
        if (owner == f.thread && f.hosted.load(std::memory_order_relaxed)) { boost(4); break; }
  }
  return false;
}

// E3: copias pegadas de OSSetCurrentContext / OSGetCurrentThread.
bool nwii_s2_snd(CPUContext &ctx) {
  using namespace nwii::runtime::s2;
  return &ctx != g_main;
}
uint32_t nwii_s2_thr(void) {
  using namespace nwii::runtime::s2;
  return snd_cur_thread();
}
}
