#include <unordered_set>
#include "runtime/nwii_env.h"
#include <glad/glad.h>

#include <map>
#include <deque>
#include <array>
#include <algorithm>
#include <unordered_map>
#include <memory>
#include "runtime/cpu_context.h"
#include "../core/s2.h"  // r38c: pc_sched (PC), vigia

namespace nwii::runtime { extern CPUContext *g_ctx_ptr; }

#include "runtime/gx/fifo_parser.h"
#include "runtime/texture_cache_gl.h"
#include "runtime/gx/renderer.h"
#include "runtime/hw/hw.h"
#include "runtime/gx_state.h"
#include "runtime/cpu_context.h"
#include <iostream>
#include <cstdlib>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <condition_variable>
#if defined(__SWITCH__)
#include <switch.h>
#include <pthread.h>
#include "../platform/switch/switch_platform.h"
#endif
#include <iterator>
#include <chrono>
#include <cstring>
#include <cstdio>

using namespace nwii::runtime;
using namespace nwii::runtime::gx;

GXState nwii::runtime::gx::g_state = {};

static std::unique_ptr<nwii::runtime::gx::IRenderer> g_renderer = nullptr;
static void* g_window = nullptr;

void GX_SetWindow(void* window) { g_window = window; }

void GX_Init() {
    g_renderer = nwii::runtime::gx::IRenderer::Create();
    if (g_renderer) g_renderer->Initialize(nullptr);
}

namespace nwii::runtime {
    extern MMU* g_mmu;
}

// R6 perf: the guest writes the GX FIFO a few bytes at a time (every
// GXPosition/GXColor call) and took a pthread mutex for each write (2.6% of
// all samples, Switch-like CPU profile). The host holds the lock only to take
// the bytes, so a test-and-test-and-set spinlock is enough.
// r38c: `owner` / `waiters` solo para el vigilante de cuelgues (quien lo tiene, cuantos esperan).
struct FifoLock {
    std::atomic<bool> busy{false};
    std::atomic<uint32_t> owner{0}, waiters{0};
    void lock() {
        for (unsigned spins = 0; busy.exchange(true, std::memory_order_acquire);) {
            if (spins > 64) waiters.fetch_add(1, std::memory_order_relaxed);
            while (busy.load(std::memory_order_relaxed)) {
                if (++spins > 64) std::this_thread::yield();
#if defined(__x86_64__) || defined(__i386__)
                else __builtin_ia32_pause();
#endif
            }
            if (spins > 64) waiters.fetch_sub(1, std::memory_order_relaxed);
        }
        owner.store(self_tag(), std::memory_order_relaxed);
    }
    void unlock() { busy.store(false, std::memory_order_release); }
    static uint32_t self_tag() {
#ifdef __SWITCH__
        return threadGetCurHandle();
#else
        return (uint32_t)std::hash<std::thread::id>{}(std::this_thread::get_id());
#endif
    }
};
FifoLock g_fifo_mutex;
std::vector<uint8_t> g_hw_fifo;

// ---- R8 memory: bounded GX backlog ------------------------------------------
// The guest never waits for the host GPU (early GX sync: an infinitely fast
// GPU), so while the host thread is stuck in one Render() -- a shader compile
// takes ~2 s on the Switch, a burst of them 45 s in citron -- the guest keeps
// queueing frames. The host then parsed the whole backlog at once (hundreds of
// frames, each display list expanded into its own commands: 550 MB of command
// vectors in citron, 2.6 GB in use after 2 min) and the vectors kept that
// capacity for good, since newlib never returns heap to the system.
// A real GP FIFO is a fixed-size ring: when it fills, the CPU waits for the
// GPU. Here the guest waits at its XFB copy once NWII_FIFO_MAX_FRAMES (default
// 8) whole frames are queued that the host has not taken, and its clock stands
// still meanwhile (the wait is added to the governor's slip), so for the game
// the stall never happened. A host that keeps up takes the FIFO every 1 ms and
// never gets there. 0 disables it.
static uint32_t s_fifo_frames = 0;  // XFB copies queued in g_hw_fifo (g_fifo_mutex)
// R11: XFB copies the render worker took from g_hw_fifo whose frame the render
// thread has not taken yet (g_fifo_mutex; 0 without the worker). The guest
// waits on the sum, so frames held by the worker still count as "not taken by
// the host" and a stalled renderer stops the guest as before.
static uint32_t s_held_frames = 0;
// ---- R14 R5: pipeline timing ([PIPE] every 2 s, render thread) ---------------
// Where a frame's time goes between the game's XFB copy (frame end handed to
// the FIFO), the worker's hand-over, the render thread taking and drawing it
// and the buffer swap. Monotonic ns; game-side figures are atomics (written
// under g_fifo_mutex by the guest thread).
static inline int64_t PipeNow() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
// ---- r37v (frente V): NWII_V_TL=<ruta> linea de tiempo de la tuberia ----------
// Anillo de sucesos con hora (PipeNow, ns) de todos los hilos: retrazo VI (disparo y si se
// lanzo la interrupcion), interrupciones entregadas al invitado, tramos ociosos del hilo del
// juego, copia XFB, trabajador listo, render toma/acaba, swap. El hilo de render vuelca los
// nuevos cada ~2 s (PrintProf) a <ruta> en binario: registros de 16 bytes <q t, I kind, I arg>.
// Solo diagnostico: apagado no cuesta mas que una carga y un salto por suceso.
namespace vtl {
struct Ev { int64_t t; uint32_t kind; uint32_t arg; };
static constexpr uint64_t kN = 1u << 16;
static Ev s_ring[kN];
static std::atomic<uint64_t> s_head{0};
static uint64_t s_tail = 0;   // hilo de render
static FILE *s_f = nullptr;
static const char *s_path = nullptr;
static const char *Path() { return s_path; }
bool g_on = false;
static inline void NoteAt(int64_t t, uint32_t kind, uint32_t arg) {
    const uint64_t i = s_head.fetch_add(1, std::memory_order_relaxed);
    Ev &e = s_ring[i & (kN - 1)];
    e.t = t; e.kind = kind; e.arg = arg;
}
static void Dump() {  // hilo de render
    if (!g_on) return;
    if (!s_f) {
        s_f = std::fopen(Path(), "wb");
        if (!s_f) { g_on = false; return; }
        std::printf("[VTL] linea de tiempo -> %s\n", Path());
    }
    const uint64_t h = s_head.load(std::memory_order_acquire);
    const uint64_t end = h > 64 ? h - 64 : 0;  // los ultimos pueden estar a medio escribir
    if (end <= s_tail) return;
    uint64_t from = s_tail;
    if (end - from > kN - 256) from = end - (kN - 256);  // desbordado: se pierden los viejos
    for (uint64_t i = from; i < end; ++i) std::fwrite(&s_ring[i & (kN - 1)], sizeof(Ev), 1, s_f);
    std::fflush(s_f);
    s_tail = end;
}
}  // namespace vtl
namespace nwii::runtime {
extern bool g_vtl_on_ios;
extern uint64_t g_v_held_n, g_v_held_idle_n, g_v_held_to_n, g_v_held_drop_n, g_v_held_us;  // ios.cpp
}
// main.cpp, con el entorno ya aplicado (en la Switch, debug.toml), antes del hilo de la CPU.
void vtl_init() {
    const char *e = std::getenv("NWII_V_TL");
    if (!(e && e[0] && e[0] != '0')) return;
    vtl::s_path = e;
    vtl::g_on = true;
    nwii::runtime::g_vtl_on_ios = true;
}
// Usado desde ios.cpp / os_hle_stubs.cpp / main.cpp (declaracion extern alli).
void vtl_note(uint32_t kind, uint32_t arg) {
    if (__builtin_expect(vtl::g_on, 0)) vtl::NoteAt(PipeNow(), kind, arg);
}
// Hilo del juego, en cada vuelta de idle_wait: tramos ociosos (fin = ultima vuelta antes de
// un hueco de mas de 300 us sin idle_wait).
void vtl_idle_turn(bool at_end) {
    if (__builtin_expect(!vtl::g_on, 1)) return;
    static int64_t last = 0;
    const int64_t t = PipeNow();
    if (!at_end && t - last > 300000) {
        if (last) vtl::NoteAt(last, 4, 0);  // fin de tramo ocioso
        vtl::NoteAt(t, 3, 0);               // empieza tramo ocioso
    }
    last = t;
}
static std::atomic<int64_t> s_pipe_xfb_last{0};
static std::atomic<uint64_t> s_pipe_xfb_n{0}, s_pipe_xfb_sum{0}, s_pipe_xfb_max{0};
static std::atomic<uint32_t> s_pipe_xfb_h[5];  // interval <=18, <=22, <=30, <=36, >36 ms
// r37v (frente V), SOLO PRUEBAS EN PC: NWII_V_SLOWTEST=<base_us>,<azar_us>: tras cada copia XFB el hilo del
// juego gira base + azar(0..azar) us (simula un juego lento como el de la Switch en un PC rapido).
static bool s_v_slow_due = false;
static NWII_NOINLINE void VSlowTest() {
    static long base = -1, jit = 0;
    static uint32_t rnd = 12345;
    if (base < 0) {
        const char *e = std::getenv("NWII_V_SLOWTEST");
        base = 0;
        if (e) std::sscanf(e, "%ld,%ld", &base, &jit);
    }
    if (base <= 0 && jit <= 0) return;
    rnd = rnd * 1103515245u + 12345u;
    const long us = base + (jit > 0 ? (long)((rnd >> 8) % (uint32_t)jit) : 0);
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < end) {}
}
static std::atomic<uint64_t> s_c38_xfb{0};  // r38c: copias XFB del juego (vigia)
static inline void PipeNoteXfb() {  // guest thread, g_fifo_mutex held
    s_c38_xfb.fetch_add(1, std::memory_order_relaxed);
    const int64_t t = PipeNow();
    static uint32_t s_vtl_xfb = 0;
    s_v_slow_due = true;  // r37v NWII_V_SLOWTEST
    if (__builtin_expect(vtl::g_on, 0)) vtl::NoteAt(t, 5, ++s_vtl_xfb);
    const int64_t l = s_pipe_xfb_last.exchange(t, std::memory_order_relaxed);
    if (!l) return;
    const uint64_t d = (uint64_t)(t - l);
    s_pipe_xfb_n.fetch_add(1, std::memory_order_relaxed);
    s_pipe_xfb_sum.fetch_add(d, std::memory_order_relaxed);
    if (d > s_pipe_xfb_max.load(std::memory_order_relaxed)) s_pipe_xfb_max.store(d, std::memory_order_relaxed);
    const double ms = d / 1e6;
    s_pipe_xfb_h[ms <= 18 ? 0 : ms <= 22 ? 1 : ms <= 30 ? 2 : ms <= 36 ? 3 : 4].fetch_add(
        1, std::memory_order_relaxed);
}
// worker: frame end (newest XFB copy) -> hand-over
static std::atomic<uint64_t> s_pipe_w_n{0}, s_pipe_w_sum{0}, s_pipe_w_max{0};
// render thread (plain): hand-over -> taken, render time, swap time, loop period
static uint64_t s_pipe_r_n = 0, s_pipe_r_wait = 0, s_pipe_r_wait_max = 0, s_pipe_r_draw = 0,
                s_pipe_r_draw_max = 0, s_pipe_s_n = 0, s_pipe_s_sum = 0, s_pipe_s_max = 0,
                s_pipe_l_n = 0, s_pipe_l_sum = 0, s_pipe_l_max = 0;
static std::atomic<std::thread::id> s_host_tid{};  // the thread that drains (ProcessGXFifo)
static std::atomic<uint64_t> s_fifo_waits{0}, s_fifo_wait_us{0};
static uint32_t FifoMaxFrames() {
    static const uint32_t n = [] {
        const char *e = NWII_ENV("NWII_FIFO_MAX_FRAMES");
        return e ? (uint32_t)std::strtoul(e, nullptr, 10) : 8u;
    }();
    return n;
}
// Guest thread, at an XFB copy, without g_fifo_mutex held.
static NWII_NOINLINE void FifoWaitForHost() {
    nwii::runtime::CPUContext *ctx = nwii::runtime::g_ctx_ptr;
    if (!ctx || std::this_thread::get_id() == s_host_tid.load(std::memory_order_relaxed))
        return;
    const uint32_t max = FifoMaxFrames();
    const bool wall = nwii::runtime::CPUContext::wall_clock_tb();
    const uint64_t w0 = wall ? ctx->wall_timebase_now() : 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        {
            std::lock_guard<FifoLock> lock(g_fifo_mutex);
            if (s_fifo_frames + s_held_frames < max) break;
        }
        if (!ctx->is_running.load(std::memory_order_relaxed)) break;
        // A host that has not drained for this long is not coming back soon
        // (or is gone): carry on as before rather than wait forever. The next
        // XFB copy waits again.
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) break;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count();
    s_fifo_waits.fetch_add(1, std::memory_order_relaxed);
    s_fifo_wait_us.fetch_add(us, std::memory_order_relaxed);
    vtl_note(10, (uint32_t)us);
    if (wall) {
        // Guest time stands still while it waits: the governor's clock
        // mapping (guest = wall - slip) absorbs the wait, and the wait counts
        // neither as busy time nor as time to slip at a reduced rate.
        const uint64_t d = ctx->wall_timebase_now() - w0;
        ctx->gov_slip += d;
        ctx->gov_wall += d;
        ctx->gov_idle_wall += d;
        ctx->gov_step_wall += d;
        ctx->tb_invalidate();
    }
}

// Display lists are read out of guest RAM when the FIFO is parsed, which is
// long after the game issued the call -- and the game double-buffers those
// buffers, so by then the bytes can already belong to the next frame. The
// GXCallDisplayList seam copies them at the moment of the call; the parser
// takes the copy when one exists.
static std::mutex g_dl_snap_mtx;
// Snapshots are shared (not copied) with the parser. The map used to be
// cleared at 1024 entries; a busy 3D scene (Peach's party: ~650 display-list
// calls per frame) crossed that every 1-2 frames, so the parser often missed
// the snapshot and read live guest RAM instead. Keyed per list, so its size is
// bounded by the number of distinct lists; the cap is only a safety net.
static std::unordered_map<uint64_t, std::shared_ptr<const std::vector<uint8_t>>> g_dl_snap;

namespace nwii::runtime {
void GX_DL_Snapshot(uint32_t addr, uint32_t size) {
    if (!nwii::runtime::g_mmu || size == 0 || size > (8u << 20)) return;
    auto buf = std::make_shared<std::vector<uint8_t>>(size);
    if (const uint8_t* p = nwii::runtime::g_mmu->get_ptr(0x80000000u | (addr & 0x1FFFFFFF)))
        std::memcpy(buf->data(), p, size);
    else
        for (uint32_t i = 0; i < size; ++i) (*buf)[i] = nwii::runtime::g_mmu->read8(addr + i);
    std::lock_guard<std::mutex> lk(g_dl_snap_mtx);
    if (g_dl_snap.size() > 4096) g_dl_snap.clear();
    g_dl_snap[((uint64_t)(addr & 0x1FFFFFFF) << 24) ^ size] = std::move(buf);
}

bool GX_DL_Fetch(uint32_t addr, uint32_t size, std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lk(g_dl_snap_mtx);
    auto it = g_dl_snap.find(((uint64_t)(addr & 0x1FFFFFFF) << 24) ^ size);
    if (it == g_dl_snap.end() || it->second->size() != size) return false;
    out = *it->second;
    return true;
}

// Copy-free variant for the FIFO parser.
std::shared_ptr<const std::vector<uint8_t>> GX_DL_FetchShared(uint32_t addr, uint32_t size) {
    std::lock_guard<std::mutex> lk(g_dl_snap_mtx);
    auto it = g_dl_snap.find(((uint64_t)(addr & 0x1FFFFFFF) << 24) ^ size);
    if (it == g_dl_snap.end() || it->second->size() != size) return nullptr;
    return it->second;
}

// ---- Indexed matrix loads (LOAD_INDX) at the moment the game issues them ----
// LOAD_INDX makes the GPU read a matrix from a guest RAM array (CP array 12-15,
// set by GXSetArray) when it executes the command, right after the CPU wrote
// it. The parser runs up to a few frames later, and reading the array then
// gives whatever the game has written into it since: the title screen's wipe
// frames draw the scene twice per frame with two cameras into the same G3D
// matrix array, and the visible pass got the other (off-screen) camera, so the
// iris showed black. The guest thread now copies the data when it writes the
// command (main stream) or calls the display list (lists), in stream order;
// the parser takes the copies in the same order, checks they belong to the
// command it is parsing (same array/address/size or same list), and reads
// live RAM as before when there is none. NWII_IDXSNAP=0 disables.
// [r44w] oculto: el código no pasa por la GOT para leerlo (una carga menos en cada escritura al pipe)
namespace hw { extern bool g_wgp_redirect_on __attribute__((visibility("hidden"))); }  // hw_pi.cpp
static const bool s_idx_on = [] {
    const char *e = NWII_ENV("NWII_IDXSNAP");
    return !(e && e[0] == '0');
}();
struct IdxRecMain { uint32_t frame, arr, addr, num; float v[16]; };
// The parser's copy of the record of the list it is expanding (parser thread;
// the byte vectors keep their capacity from call to call).
struct IdxRecDL {
    uint32_t frame;
    uint64_t key;
    uint32_t base[4];        // sniffed array base (0 = unknown)
    uint32_t lo[4];          // copied byte range [base+lo, base+lo+bytes.size())
    std::vector<uint8_t> bytes[4];
};
struct IdxRange { uint32_t lo = ~0u, hi = 0; };
static std::atomic<uint64_t> s_idx_bytes{0};  // bytes the guest thread copied for lists
static constexpr uint32_t kIdxMaxSpan = 64u << 10;
static constexpr size_t kIdxMaxQueue = 16384, kIdxSearch = 64;

// ---- R16w: the records go from the guest thread to the parser without a lock
// r13 kept them in two std::deque and a std::unordered_map under one
// std::mutex, taken twice per GXCallDisplayList by the guest (~22 000 calls/s
// in the title demo) and twice per list by the parser on the render worker.
// The worker runs on core 2 at a lower priority than the AX mixer, which
// preempts it, sometimes with the lock held; the guest then blocked in the
// kernel (svcArbitrateLock: 4 % of the CPU thread's samples on the console)
// and every record cost heap allocations (one vector per array) freed on the
// other thread. Now:
//   - main-stream and list records go through single-producer /
//     single-consumer rings (the guest pushes at the tail, the parser takes
//     from the head, both with acquire/release indices);
//   - the array bytes of a list record live in a byte ring, contiguous per
//     record; the parser copies them into its IdxRecDL when it takes the
//     record and gives the space back with the head;
//   - the ranges each list read the last time (s_idx_need) belong to the
//     parser, which sends the guest every change through a third ring; the
//     guest applies them to its own copy before each lookup, so it sees every
//     change made before its call, as it did under the lock.
// What the parser takes is unchanged: the same records in the same order, the
// same search window (kIdxSearch) and the same pruning by frame. Differences,
// only past the caps: a full ring drops the new record (r13 dropped the
// oldest) and a full byte ring sends the record without bytes; either way the
// parser reads live RAM for it, as for any record it lacks.
template <class T> struct IdxRing {
    T *v = nullptr;
    uint32_t mask = 0;
    alignas(64) std::atomic<uint64_t> tail{0};  // producer
    alignas(64) std::atomic<uint64_t> head{0};  // consumer
    explicit IdxRing(uint32_t cap) : v(new T[cap]), mask(cap - 1) {}
};
struct IdxRecDLR {  // a list record in the ring; its bytes: [bpos, bpos + sum(len))
    uint32_t frame;
    uint64_t key;
    uint32_t base[4], lo[4], len[4];
    uint32_t boff[4];     // where each array's bytes start, from bpos
    uint64_t bpos, bend;  // byte ring positions (bend: the ring position after it)
};
struct IdxNeedMsg { uint64_t key; std::array<IdxRange, 4> r; bool clear; };
static constexpr uint32_t kIdxBytesCap = 1u << 20;  // byte ring (~60 frames of the title demo)
struct IdxRings {
    IdxRing<IdxRecMain> main{(uint32_t)kIdxMaxQueue};
    IdxRing<IdxRecDLR> dl{(uint32_t)kIdxMaxQueue};
    IdxRing<IdxNeedMsg> need{4096};
    uint8_t *bytes = new uint8_t[kIdxBytesCap];
    uint64_t bw = 0;                          // guest: next byte position
    alignas(64) std::atomic<uint64_t> br{0};  // parser: bytes before this are free
};
static IdxRings *const s_idx_rings = s_idx_on ? new IdxRings : nullptr;
// Guest thread: its copy of the parser's ranges per list.
static std::unordered_map<uint64_t, std::array<IdxRange, 4>> s_idx_need_g;
// Parser thread: the ranges per list (sent to the guest when they change).
static std::unordered_map<uint64_t, std::array<IdxRange, 4>> s_idx_need;
static uint64_t s_idx_drops = 0;  // guest: records dropped at a full ring

// Guest thread: the CP array state seen in the pipe.
static uint32_t s_sn_base[4], s_sn_stride[4];
static uint32_t s_sn_frame = 0;  // XFB copies written (stamps the records)

static bool idx_copy(uint32_t phys, uint32_t len, uint8_t *dst) {
    phys &= 0x1FFFFFFF;
    const bool m1 = phys + len <= 0x01800000u;
    const bool m2 = phys >= 0x10000000u && phys + len <= 0x14000000u;
    if (!(m1 || m2) || !g_mmu) return false;
    if (const uint8_t *p = g_mmu->get_ptr(0x80000000u | phys)) std::memcpy(dst, p, len);
    else for (uint32_t i = 0; i < len; ++i) dst[i] = g_mmu->read8(0x80000000u | (phys + i));
    return true;
}

static inline bool idx_in_ram(uint32_t phys, uint32_t len) {
    phys &= 0x1FFFFFFF;
    return phys + len <= 0x01800000u || (phys >= 0x10000000u && phys + len <= 0x14000000u);
}
static void idx_dl_call(uint32_t addr, uint32_t size);

// The SDK writes these commands as separate calls: CP = Write8(0x08),
// Write8(reg), Write32(value); LOAD_INDX = Write8(0x20/0x28/0x30/0x38),
// Write32; CALL_DL = Write8(0x40), Write32(addr), Write32(size). A small
// state machine over the calls finds them (the seams' 0x78 marker, Write8 x3
// + Write32 x2, is skipped). Data that merely looks like one makes a record
// the parser never takes: it only takes records that match the command it
// is parsing.
static int s_sn_st = 0;   // 1 0x08, 2 0x08 reg, 3 LOAD_INDX op, 4 0x40, 5 0x40 addr, 6/7 0x78 marker
static uint32_t s_sn_a = 0;

static void idx_loadindx(uint8_t op, uint32_t v) {
    if (!g_mmu) return;
    const uint32_t arr = (op - 0x20) / 8, num = ((v >> 12) & 0xF) + 1;
    const uint32_t addr = s_sn_base[arr] + (v >> 16) * s_sn_stride[arr];
    if (!s_sn_base[arr] || !idx_in_ram(addr, num * 4)) return;
    IdxRing<IdxRecMain> &q = s_idx_rings->main;
    const uint64_t t = q.tail.load(std::memory_order_relaxed);
    if (t - q.head.load(std::memory_order_acquire) > q.mask) { ++s_idx_drops; return; }
    IdxRecMain &r = q.v[t & q.mask];
    r.frame = s_sn_frame;
    r.arr = arr;
    r.num = num;
    r.addr = addr;
    for (uint32_t i = 0; i < num; ++i) r.v[i] = g_mmu->read_f32(addr + i * 4);
    q.tail.store(t + 1, std::memory_order_release);
}
static inline void idx_sniff8(uint8_t b) {
    if (!s_idx_on) return;
    switch (s_sn_st) {
    case 1: s_sn_st = 2; s_sn_a = b; return;
    case 6: s_sn_st = 7; return;
    case 7: s_sn_st = 0; return;
    default: break;
    }
    s_sn_st = b == 0x08 ? 1 : (b & 0xE7) == 0x20 ? 3 : b == 0x40 ? 4 : b == 0x78 ? 6 : 0;
    s_sn_a = b;
}
static inline void idx_sniff16(uint16_t) { s_sn_st = 0; }
static inline void idx_sniff32(uint32_t v, bool is_int) {
    const int st = s_sn_st;
    s_sn_st = 0;
    if (!is_int || !s_idx_on) return;
    if (st == 2) {
        if (s_sn_a >= 0xAC && s_sn_a <= 0xAF) s_sn_base[s_sn_a - 0xAC] = v & 0x3FFFFFFF;
        else if (s_sn_a >= 0xBC && s_sn_a <= 0xBF) s_sn_stride[s_sn_a - 0xBC] = v & 0xFF;
    } else if (st == 3) {
        idx_loadindx((uint8_t)s_sn_a, v);
    } else if (st == 4) {
        s_sn_st = 5;
        s_sn_a = v;
    } else if (st == 5) {
        const uint32_t addr = s_sn_a, size = v;
        if (size && size <= (8u << 20) && !((addr | size) & 31) && idx_in_ram(addr, size))
            idx_dl_call(addr, size);
    }
}

// GXCallDisplayList, guest thread: copy the parts of the arrays this list
// read the previous times it was parsed (unknown the first time: live RAM).
static void idx_need_drain() {  // guest: apply the parser's range changes
    IdxRing<IdxNeedMsg> &q = s_idx_rings->need;
    uint64_t h = q.head.load(std::memory_order_relaxed);
    const uint64_t t = q.tail.load(std::memory_order_acquire);
    if (h == t) return;
    for (; h != t; ++h) {
        const IdxNeedMsg &m = q.v[h & q.mask];
        if (m.clear) s_idx_need_g.clear();
        else s_idx_need_g[m.key] = m.r;
    }
    q.head.store(h, std::memory_order_release);
}
static void idx_dl_call(uint32_t addr, uint32_t size) {
    if (!s_idx_on || hw::g_wgp_redirect_on) return;
    IdxRings &R = *s_idx_rings;
    IdxRing<IdxRecDLR> &q = R.dl;
    const uint64_t t = q.tail.load(std::memory_order_relaxed);
    if (t - q.head.load(std::memory_order_acquire) > q.mask) { ++s_idx_drops; return; }
    idx_need_drain();
    IdxRecDLR &r = q.v[t & q.mask];
    r.frame = s_sn_frame;
    r.key = ((uint64_t)(addr & 0x1FFFFFFF) << 24) ^ size;
    std::array<IdxRange, 4> need{};
    {
        auto it = s_idx_need_g.find(r.key);
        if (it != s_idx_need_g.end()) need = it->second;
    }
    uint32_t total = 0;
    for (int a = 0; a < 4; ++a) {
        r.base[a] = s_sn_base[a];
        r.lo[a] = 0;
        r.len[a] = 0;
        r.boff[a] = total;
        if (!r.base[a] || need[a].hi <= need[a].lo) continue;
        r.lo[a] = need[a].lo;
        r.len[a] = need[a].hi - need[a].lo;
        total += r.len[a];
    }
    if (total) s_idx_bytes.fetch_add(total, std::memory_order_relaxed);
    // Room for the bytes, contiguous (skip the end of the ring if needed).
    uint64_t pos = R.bw;
    const uint32_t off = (uint32_t)(pos & (kIdxBytesCap - 1));
    if (off + total > kIdxBytesCap) pos += kIdxBytesCap - off;
    if (total > kIdxBytesCap || pos + total - R.br.load(std::memory_order_acquire) > kIdxBytesCap) {
        for (int a = 0; a < 4; ++a) r.len[a] = 0;  // no room: the parser reads live RAM
        pos = R.bw;
        total = 0;
    }
    r.bpos = pos;
    uint8_t *const dst = R.bytes + (pos & (kIdxBytesCap - 1));
    for (int a = 0; a < 4; ++a)
        if (r.len[a] && !idx_copy(r.base[a] + r.lo[a], r.len[a], dst + r.boff[a]))
            r.len[a] = 0;  // as r13: no bytes (the space stays taken)
    R.bw = pos + total;
    r.bend = R.bw;
    q.tail.store(t + 1, std::memory_order_release);
}

// ---- parser side (host thread) ----
// Records the parser never matched (data that looked like a command, or a
// command it parsed differently) are dropped once they are 16 frames older
// than the newest one: the host is never that far behind (FIFO_MAX_FRAMES).
template <class T> static uint64_t idx_prune(IdxRing<T> &q, uint64_t h, uint64_t t) {
    if (h == t) return h;
    const uint32_t newest = q.v[(t - 1) & q.mask].frame;
    while (h != t && q.v[h & q.mask].frame + 16 < newest) ++h;
    return h;
}
// NWII_IDXSNAP_LOG=1: [IDXSNAP] counters (copies used / live reads).
static uint64_t s_idx_n[6];  // main used, main live, dl calls with/without record, dl used, dl live
static void idx_log() {
    static const bool on = NWII_ENV("NWII_IDXSNAP_LOG") != nullptr;
    static uint64_t next = 2000;
    if (!on || s_idx_n[2] + s_idx_n[3] < next) return;
    next += 20000;
    std::printf("[IDXSNAP] main used=%llu live=%llu | lists with=%llu without=%llu | in lists used=%llu live=%llu | copied %llu KB | dropped %llu\n",
                (unsigned long long)s_idx_n[0], (unsigned long long)s_idx_n[1],
                (unsigned long long)s_idx_n[2], (unsigned long long)s_idx_n[3],
                (unsigned long long)s_idx_n[4], (unsigned long long)s_idx_n[5],
                (unsigned long long)(s_idx_bytes.load(std::memory_order_relaxed) >> 10),
                (unsigned long long)s_idx_drops);
}
// Main-stream LOAD_INDX: the copy made when it was written, if it is this one.
bool GX_IdxTakeMain(int arr, uint32_t addr, int num, float *out) {
    if (!s_idx_on) return false;
    IdxRing<IdxRecMain> &q = s_idx_rings->main;
    const uint64_t t = q.tail.load(std::memory_order_acquire);
    const uint64_t h0 = q.head.load(std::memory_order_relaxed);
    const uint64_t h = idx_prune(q, h0, t);
    const uint64_t n = std::min<uint64_t>(t - h, kIdxSearch);
    for (uint64_t i = 0; i < n; ++i) {
        const IdxRecMain &r = q.v[(h + i) & q.mask];
        if (r.arr == (uint32_t)arr && r.addr == addr && r.num == (uint32_t)num) {
            std::memcpy(out, r.v, num * sizeof(float));
            q.head.store(h + i + 1, std::memory_order_release);
            ++s_idx_n[0];
            return true;
        }
    }
    if (h != h0) q.head.store(h, std::memory_order_release);
    ++s_idx_n[1];
    return false;
}

static bool s_idx_ctx = false;
static IdxRecDL s_idx_cur;
static std::array<IdxRange, 4> s_idx_seen;
// The list ring's head moves to `h`: the bytes of the records before it are free.
static void idx_dl_release(uint64_t h) {
    IdxRings &R = *s_idx_rings;
    R.br.store(R.dl.v[(h - 1) & R.dl.mask].bend, std::memory_order_release);
    R.dl.head.store(h, std::memory_order_release);
}
// CALL_DL in the main stream: take that call's record for the list's LOAD_INDX.
void GX_IdxBeginDL(uint32_t addr, uint32_t size) {
    s_idx_ctx = false;
    if (!s_idx_on) return;
    const uint64_t key = ((uint64_t)(addr & 0x1FFFFFFF) << 24) ^ size;
    IdxRings &R = *s_idx_rings;
    IdxRing<IdxRecDLR> &q = R.dl;
    const uint64_t t = q.tail.load(std::memory_order_acquire);
    const uint64_t h0 = q.head.load(std::memory_order_relaxed);
    const uint64_t h = idx_prune(q, h0, t);
    const uint64_t n = std::min<uint64_t>(t - h, kIdxSearch);
    for (uint64_t i = 0; i < n; ++i) {
        const IdxRecDLR &r = q.v[(h + i) & q.mask];
        if (r.key != key) continue;
        s_idx_cur.frame = r.frame;
        s_idx_cur.key = r.key;
        const uint8_t *src = R.bytes + (r.bpos & (kIdxBytesCap - 1));
        for (int a = 0; a < 4; ++a) {
            s_idx_cur.base[a] = r.base[a];
            s_idx_cur.lo[a] = r.lo[a];
            s_idx_cur.bytes[a].assign(src + r.boff[a], src + r.boff[a] + r.len[a]);
        }
        idx_dl_release(h + i + 1);
        s_idx_ctx = true;
        s_idx_seen = {};
        ++s_idx_n[2];
        idx_log();
        return;
    }
    if (h != h0) idx_dl_release(h);
    ++s_idx_n[3];
    idx_log();
}

// LOAD_INDX inside the list being expanded.
bool GX_IdxReadDL(int arr, uint32_t addr, int num, float *out) {
    if (!s_idx_ctx || !s_idx_cur.base[arr]) return false;
    const uint32_t off = addr - s_idx_cur.base[arr], len = (uint32_t)num * 4;
    if (off >= kIdxMaxSpan || off + len > kIdxMaxSpan) return false;
    IdxRange &sr = s_idx_seen[arr];
    sr.lo = std::min(sr.lo, off);
    sr.hi = std::max(sr.hi, off + len);
    const std::vector<uint8_t> &b = s_idx_cur.bytes[arr];
    const uint32_t lo = s_idx_cur.lo[arr];
    if (off < lo || off + len > lo + b.size()) { ++s_idx_n[5]; return false; }
    ++s_idx_n[4];
    for (int i = 0; i < num; ++i) {
        const uint8_t *p = &b[off - lo + i * 4];
        const uint32_t u = (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
        std::memcpy(&out[i], &u, 4);
    }
    return true;
}

// Parser: queue a range change for the guest. False: the ring is full (the
// caller leaves its own map as it was, so the change is sent again later).
static bool idx_need_send(uint64_t key, const std::array<IdxRange, 4> &r, bool clear) {
    IdxRing<IdxNeedMsg> &q = s_idx_rings->need;
    const uint64_t t = q.tail.load(std::memory_order_relaxed);
    if (t - q.head.load(std::memory_order_acquire) > q.mask) return false;
    IdxNeedMsg &m = q.v[t & q.mask];
    m.key = key;
    m.r = r;
    m.clear = clear;
    q.tail.store(t + 1, std::memory_order_release);
    return true;
}

// End of the list: remember the range it read for its next call.
void GX_IdxEndDL() {
    if (!s_idx_ctx) return;
    s_idx_ctx = false;
    bool any = false;
    for (int a = 0; a < 4; ++a) any |= s_idx_seen[a].hi > s_idx_seen[a].lo;
    if (!any) return;
    // As r13: past 8192 lists the map starts again (and so does the guest's).
    if (s_idx_need.size() >= 8192) {
        if (!idx_need_send(0, {}, true)) return;
        s_idx_need.clear();
    }
    auto it = s_idx_need.try_emplace(s_idx_cur.key).first;
    std::array<IdxRange, 4> nr = it->second;
    bool changed = false;
    for (int a = 0; a < 4; ++a) {
        const IdxRange &sr = s_idx_seen[a];
        if (sr.hi <= sr.lo) continue;
        const uint32_t lo = std::min(nr[a].lo, sr.lo), hi = std::max(nr[a].hi, sr.hi);
        changed |= lo != nr[a].lo || hi != nr[a].hi;
        nr[a].lo = lo;
        nr[a].hi = hi;
    }
    if (changed && idx_need_send(s_idx_cur.key, nr, false)) it->second = nr;
}
} // namespace nwii::runtime


static std::vector<nwii::runtime::gx::GXCommand> s_carry;
// FIFO bytes taken from the guest and not parsed yet (a partial command), and
// the frame handed to Render() with the carried-over tail of the next one.
// File scope (they were function statics) so the R8 memory report sees them.
static std::vector<uint8_t> s_parse_buf;
static std::vector<nwii::runtime::gx::GXCommand> s_frame, s_tail;
// R7 perf: positions in s_carry of its draw commands, collected by the
// frame-end scan (which reads every new command anyway) so the decode pass
// does not walk the whole frame again to find them.
static std::vector<uint32_t> s_carry_draws;
// R22 R7: per draw of s_carry_draws, its prepared entry when the parser made it
// (display-list draw memo) or FifoParser::kEntryUnknown; and the parser's index.
static std::vector<const void *> s_carry_ents;
static nwii::runtime::gx::FifoParser::Index s_pindex;
static bool PIndexOn() {  // NWII_PINDEX=0: the parser does not index (old scan, no draw memo)
    static const bool on = [] { const char *e = NWII_ENV("NWII_PINDEX"); return !(e && e[0] == '0'); }();
    return on;
}

uint64_t g_stat_frames = 0;
uint64_t g_stat_draws = 0;


uint64_t g_stat_parse_us = 0;
uint64_t g_stat_render_us = 0;
// [RPROF] (printed with [GXPROF]): host Render() cost per presented frame.
// Wall time (render_us) moves with machine load and GL driver waits; the
// thread CPU time (cpu_us) of the drain -- parse, decode, render -- is the
// number that predicts the host thread's budget on a slower machine.
static uint64_t g_prof_render_us = 0, g_prof_render_frames = 0;
// [RPROF] render_max_us: the slowest single Render() of the window (a shader
// compile shows up here as one long frame).
static uint64_t g_prof_render_max_us = 0;
// R6 perf, NWII_GPUPROF=1: GPU time of Render() per frame (GL_TIME_ELAPSED
// queries, read back a few frames later so the CPU never waits for them),
// printed as gpu_us/frame in [RPROF]. Off: no GL query at all.
static uint64_t g_prof_gpu_us = 0, g_prof_gpu_frames = 0;
namespace {
struct GpuTimer {
    static constexpr int N = 8;
    GLuint q[N] = {};
    bool busy[N] = {};
    int next = 0;
    bool on() {
        static const bool e = NWII_ENV("NWII_GPUPROF") != nullptr;
        return e;
    }
    void begin() {
        if (!q[0]) glGenQueries(N, q);
        collect();
        if (busy[next]) return;  // all in flight: skip this frame
        glBeginQuery(GL_TIME_ELAPSED, q[next]);
    }
    void end() {
        if (busy[next]) return;
        glEndQuery(GL_TIME_ELAPSED);
        busy[next] = true;
        next = (next + 1) % N;
    }
    void collect() {
        for (int i = 0; i < N; ++i) {
            if (!busy[i]) continue;
            GLint ready = 0;
            glGetQueryObjectiv(q[i], GL_QUERY_RESULT_AVAILABLE, &ready);
            if (!ready) continue;
            GLuint64 ns = 0;
            glGetQueryObjectui64v(q[i], GL_QUERY_RESULT, &ns);
            g_prof_gpu_us += ns / 1000;
            ++g_prof_gpu_frames;
            busy[i] = false;
        }
    }
} g_gpu_timer;
}
static uint64_t g_prof_render_cpu_us = 0, g_prof_drain_cpu_us = 0;
static uint64_t g_prof_dropped_frames = 0;  // frames whose draws frame-skip dropped
#include <time.h>
static inline uint64_t thread_cpu_us() {
#if defined(CLOCK_THREAD_CPUTIME_ID) && !defined(__SWITCH__)
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#else
    return 0;  // no per-thread clock (Switch newlib): cpu_us reads 0
#endif
}

void GX_GetClearColor(unsigned char rgba[4]) {
    rgba[0] = (unsigned char)(g_state.clearAR & 0xFF);
    rgba[1] = (unsigned char)(g_state.clearGB >> 8);
    rgba[2] = (unsigned char)(g_state.clearGB & 0xFF);
    rgba[3] = (unsigned char)(g_state.clearAR >> 8);
}

void GX_GetXfb(uint32_t* addr, unsigned* w, unsigned* h, unsigned* stride) {
    *addr   = g_state.xfbAddr;
    *w      = g_state.xfbW;
    *h      = g_state.xfbH;
    *stride = g_state.xfbStride;
}

namespace nwii::runtime::gx {
extern uint64_t g_gx_drain_epoch;
// R7 perf: the frame's draw commands in order (collected for decode below),
// lent to Render() for the duration of the call so it can prefetch the
// vertex arrays of the draws ahead of the one it is emitting.
extern const std::vector<GXCommand*>* g_render_draws;
const void *GX_PrepDrawForRender(const GXCommand &cmd, std::vector<uint32_t> &pack);  // renderer_gl.cpp (R14 R5)
extern const std::vector<const void *> *g_render_entries;  // renderer_gl.cpp (R14 R5)
extern const std::vector<uint32_t> *g_render_pack;         // renderer_gl.cpp (R14 R5)
extern const std::vector<GXCommand> *g_render_prefix;      // renderer_gl.cpp (r36r NWII_R_PREFIX)
extern size_t g_render_start;                              // renderer_gl.cpp (r40d NWII_D_DROPFAST)
extern const std::vector<GXCommand> *g_d_merge_alt;        // renderer_gl.cpp (r40d NWII_D_MERGE_CHECK)
}
static uint64_t g_ph[8], g_ph_n;
static std::vector<nwii::runtime::gx::GXCommand*> s_draws;
// R14 R5: per draw of s_draws, its emit-cache entry (made on this side) or null.
static std::vector<const void*> s_draw_entries;
// R14 R5: GPU XF words of the frame's non-display-list draws (renderer_gl.cpp xf::PackHdr).
static std::vector<uint32_t> s_draw_pack;
// NWII_R7PH=<pmu>: drain cost per rendered frame split by phase, in host
// thread cycles of perf PMU <pmu> (cpu_atom / cpu_core on hybrid Intel,
// cpu elsewhere; the thread must stay on that core type), or in thread CPU
// microseconds if the counter cannot be opened. Diagnostic only (it keeps
// the drain on the render thread: no render worker).
#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
static int PhOpenCycles(const char *pmu) {
    char path[128];
    std::snprintf(path, sizeof path, "/sys/bus/event_source/devices/%s/type", pmu);
    FILE *f = std::fopen(path, "r");
    if (!f) return -1;
    unsigned type = 0;
    const bool ok = std::fscanf(f, "%u", &type) == 1;
    std::fclose(f);
    if (!ok) return -1;
    perf_event_attr a{};
    a.size = sizeof a;
    a.type = PERF_TYPE_HARDWARE;
    a.config = PERF_COUNT_HW_CPU_CYCLES | ((uint64_t)type << 32);
    if (std::strcmp(pmu, "cpu") == 0) a.config = PERF_COUNT_HW_CPU_CYCLES;
    return (int)syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);
}
#endif
// (Read on first use: the Switch applies env.txt after static initialisation.)
static const char *PhEnv() {
    static const char *e = NWII_ENV("NWII_R7PH");
    return e;
}
static bool PhOn() {
    static const bool on = PhEnv() != nullptr;
    return on;
}
static int PhFd() {
    static const int fd = [] {
#if defined(__linux__)
        const char *e = PhEnv();
        if (e && *e && std::strcmp(e, "1")) return PhOpenCycles(e);
#endif
        return -1;
    }();
    return fd;
}
static uint64_t PhNow() {
#if defined(__linux__)
    uint64_t v = 0;
    if (PhFd() >= 0 && read(PhFd(), &v, sizeof v) == (ssize_t)sizeof v) return v / 1000;
#endif
    return thread_cpu_us();
}
static uint64_t s_ph_t = 0;
// ---- R22 R7: [WPROF] where the preparing side (render worker) spends its time -----
// Always on, coarse (a few tick reads per PrepFrame call; NWII_WPROF=2 also splits
// the per-draw loop into DecodeDraw / GX_PrepDrawForRender with two reads per
// draw). Printed every 2 s with [GXPROF] by the render thread, per prepared frame.
namespace wprof {
enum { kLock, kParse, kScan, kClear, kSplit, kDecode, kPrep, kHand, kN };
static const char *const kName[kN] = {"lock", "parse", "scan", "clear", "split", "decode", "prep", "hand"};
static std::atomic<uint64_t> s_acc[kN], s_frames{0}, s_calls{0}, s_cmds{0}, s_draws{0}, s_busy{0};
static std::atomic<uint64_t> s_dlhit_us{0}, s_dlhit_cmds{0}, s_dlmiss_us{0};
static std::atomic<uint64_t> s_rfree{0}, s_rfree_ns{0};  // frames the render thread freed, its time
static inline uint64_t Tick() {
#if defined(__SWITCH__)
    return armGetSystemTick();
#else
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
static inline double TickUs(uint64_t t) {
#if defined(__SWITCH__)
    return t / 19.2;
#else
    return t / 1000.0;
#endif
}
static int Level() {
    static const int l = [] { const char *e = NWII_ENV("NWII_WPROF"); return e ? std::atoi(e) : 1; }();
    return l;
}
struct Stage {  // accumulates locally, published once per PrepFrame call
    uint64_t t, a[kN] = {};
    Stage() : t(Tick()) {}
    inline void mark(int i) { const uint64_t n = Tick(); a[i] += n - t; t = n; }
    ~Stage() { publish(); }
    void publish() {
        uint64_t sum = 0;
        for (int i = 0; i < kN; ++i)
            if (a[i]) { s_acc[i].fetch_add(a[i], std::memory_order_relaxed); sum += a[i]; }
        s_busy.fetch_add(sum, std::memory_order_relaxed);
        s_calls.fetch_add(1, std::memory_order_relaxed);
        namespace fp = nwii::runtime::gx;  // (this thread's parser counters)
        if (fp::g_prof_dl_hit_us | fp::g_prof_dl_hit_cmds | fp::g_prof_dl_miss_us) {
            s_dlhit_us.fetch_add(fp::g_prof_dl_hit_us, std::memory_order_relaxed);
            s_dlhit_cmds.fetch_add(fp::g_prof_dl_hit_cmds, std::memory_order_relaxed);
            s_dlmiss_us.fetch_add(fp::g_prof_dl_miss_us, std::memory_order_relaxed);
            fp::g_prof_dl_hit_us = fp::g_prof_dl_hit_cmds = fp::g_prof_dl_miss_us = 0;
        }
    }
};
}  // namespace wprof
static inline void PH(int i) {
    if (PhOn()) { const uint64_t t = PhNow(); g_ph[i] += t - s_ph_t; s_ph_t = t; }
}

// ---- R8 memory: give back what a backlog left behind -------------------------
// The FIFO buffers and command vectors are reused drain after drain and swap
// their storage around (s_carry -> frame -> tail), so one big backlog left all
// of them at its size for good. Each drain notes how much a normal one needs
// (the largest of the last 2-4 s); a buffer holding more than 4x that (and
// over 4 MB) is reallocated to 2x it, so ordinary frame-to-frame variation never
// triggers it. Contents are kept, so nothing else changes.
namespace {
struct HighWater {
    size_t cur = 0, prev = 0;
    std::chrono::steady_clock::time_point t{};
    void note(size_t v, std::chrono::steady_clock::time_point now) {
        if (now - t >= std::chrono::seconds(2)) { prev = cur; cur = 0; t = now; }
        if (v > cur) cur = v;
    }
    size_t get() const { return cur > prev ? cur : prev; }
};
HighWater s_hw_bytes, s_hw_cmds;
uint64_t s_trims = 0, s_trimmed_bytes = 0;
template <class V>
bool TrimVec(V &v, size_t need) {
    using T = typename V::value_type;
    const size_t floor_elems = (1u << 20) / sizeof(T);
    if (need < floor_elems) need = floor_elems;
    if (v.capacity() <= 4 * need || v.capacity() <= v.size() * 2) return false;
    const size_t keep = std::max(2 * need, v.size());
    const size_t freed = (v.capacity() - keep) * sizeof(T);
    V n;
    n.reserve(keep);
    n.insert(n.end(), std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
    v.swap(n);
    ++s_trims;
    s_trimmed_bytes += freed;
    return true;
}
}  // namespace
static void TrimBuffers() {
    const size_t bytes = s_hw_bytes.get(), cmds = s_hw_cmds.get();
    // Cheap test first: this runs on every 1 ms poll.
    const size_t big = 4 * std::max<size_t>(bytes, 1u << 20);
    const size_t bigc = 4 * std::max<size_t>(cmds, (1u << 20) / sizeof(nwii::runtime::gx::GXCommand));
    if (s_parse_buf.capacity() <= big && s_carry.capacity() <= bigc &&
        s_frame.capacity() <= bigc && s_tail.capacity() <= bigc &&
        s_draws.capacity() <= bigc && s_carry_draws.capacity() <= bigc &&
        s_carry_ents.capacity() <= bigc)
        return;
    TrimVec(s_parse_buf, bytes);
    TrimVec(s_carry, cmds);
    TrimVec(s_frame, cmds);
    TrimVec(s_tail, cmds);
    TrimVec(s_draws, cmds);
    TrimVec(s_draw_entries, cmds);
    TrimVec(s_carry_draws, cmds);
    TrimVec(s_carry_ents, cmds);
}

// ---- R11 render worker -------------------------------------------------------
// The FIFO drain is split in two halves:
//   PrepFrame    (CPU only, no GL): take the FIFO bytes, parse them (display
//                lists expanded), find the frame end, frame-skip, decode the
//                surviving draws (vertex arrays read from guest RAM), and
//                hand the frame over as a FramePacket;
//   RenderPacket (GL thread): put the parser's register state in g_state
//                and Render() the frame (textures, EFB copies and the texture
//                cache keep reading guest RAM here, as before).
// With NWII_RENDER_WORKER on (default with a window), PrepFrame runs on its
// own thread (core 2 on the Switch) and prepares frame N+1 while the render
// thread draws frame N; whole frames change hands (one wake-up per frame, never
// per draw). Off (NWII_RENDER_WORKER=0, headless, debug modes), both halves run
// one after the other inside ProcessGXFifo, as before.
//
// Ownership: the parser state, the display-list and decode caches and every
// DrawRaw / DecodedVerts reference count belong to the worker (HostPtr counts
// are not atomic). The render thread only reads a packet's commands; the worker
// destroys a packet's commands only after the render thread gave it back, and
// it never touches a DecodedVerts' emit_cache (the renderer's). An emit-cache
// entry that dies on the worker gives its arena slot back on the render thread
// (renderer_gl.cpp, vbuf::ArenaFreeAny).
namespace {
struct PrepStats {
    uint64_t parse_us = 0, decode_us = 0, draws = 0, dropped = 0, prep_cpu_us = 0;
    uint64_t held = 0;  // XFB copies taken from the FIFO (worker: see s_held_frames)
    uint64_t retracts = 0;  // ready frames the worker replaced with a newer one
    uint64_t snap_draws = 0, draw_bytes = 0, dl_calls = 0, dl_bytes = 0, dl_us = 0, snap_us = 0,
             dl_loadindx = 0, dl_hits = 0, decode_hits = 0, unknown = 0;
    size_t carry = 0, buf = 0;
    void add(const PrepStats &o) {
        parse_us += o.parse_us; decode_us += o.decode_us; draws += o.draws; dropped += o.dropped;
        held += o.held; retracts += o.retracts;
        prep_cpu_us += o.prep_cpu_us; snap_draws += o.snap_draws; draw_bytes += o.draw_bytes;
        dl_calls += o.dl_calls; dl_bytes += o.dl_bytes; dl_us += o.dl_us; snap_us += o.snap_us;
        dl_loadindx += o.dl_loadindx; dl_hits += o.dl_hits; decode_hits += o.decode_hits;
        unknown += o.unknown; carry = o.carry; buf = o.buf;
    }
};
struct FramePacket {
    std::vector<nwii::runtime::gx::GXCommand> frame;
    std::vector<nwii::runtime::gx::GXCommand *> draws;  // frame's draws, in order
    std::vector<const void *> entries;  // R14 R5: draws[i]'s emit-cache entry, packed draw, or null
    std::vector<uint32_t> pack;         // R14 R5: packed draws' words
    // r36r NWII_R_PREFIX: the kept register writes of the frames retracted
    // before this one (no draws), rendered before `frame`.
    std::vector<nwii::runtime::gx::GXCommand> pre;
    // r40d NWII_D_DROPFAST: `frame` starts with the dropped part of its batch
    // (draws and dead writes, never rendered; its kept writes are in `pre`):
    // the renderer starts at frame[start].
    size_t start = 0;
    // r40d NWII_D_RENDERPREP: draws (indices into `draws`) the render thread decodes and
    // packs itself just before rendering (their entries are null until then).
    std::vector<uint32_t> rprep;
    // r40d NWII_D_MERGE_CHECK: merged draws (index into `draws`) and the vertex counts of their parts.
    std::vector<std::pair<uint32_t, std::vector<uint16_t>>> merges;
    uint64_t seq = 0;          // R22 R7: frame number (s_prep_frames) of its frame
    bool has_draws = false;    // R22 R7: its commands may point at retired vertices
    bool render_frees = false; // R22 R7: no command shares a reference count with the worker
    nwii::runtime::gx::ParseRenderState state;          // worker mode only
    PrepStats stats;
    int64_t t_ready = 0;  // R14 R5: hand-over time (PipeNow)
};
PrepStats s_prep_pending;      // preparing side: counters since the last packet
uint64_t s_prep_frames = 0;    // preparing side: frames produced
PrepStats s_prof_acc;          // render side: counters of the taken packets ([GXPROF])

// Worker hand-over. Two packets: one the render thread draws (s_rendering),
// one the worker fills; a filled one waits in s_ready until it is taken.
std::mutex s_pm;
std::condition_variable s_cv_worker, s_cv_main;
FramePacket s_pkts[3];  // r45p NWII_P_TRIPLE: the third only with it
FramePacket *s_ready = nullptr, *s_rendering = nullptr;
FramePacket *s_late_pkt = nullptr;  // r39r2 NWII_R2_LATEFREE (render thread only)
bool s_stop = false;
std::atomic<bool> s_worker_on{false};
thread_local bool t_on_worker = false;
#if defined(__SWITCH__)
pthread_t s_worker_thread;
#else
std::thread *s_worker_thread = nullptr;  // never destroyed (exit paths)
#endif
// Memory report figures of the preparing side (GX_MemReport), refreshed by the
// worker every ~2 s.
struct PrepMem {
    size_t parse_buf = 0, cmds = 0, dl_n = 0, dl_cmds = 0, dl_b = 0;
} s_prep_mem;
std::mutex s_prep_mem_m;
}  // namespace

namespace nwii::runtime::gx {
void DLCacheMemStats(size_t& entries, size_t& cmds, size_t& bytes);
}
static PrepMem ComputePrepMem() {
    using nwii::runtime::gx::GXCommand;
    PrepMem m;
    m.parse_buf = s_parse_buf.capacity();
    m.cmds = (s_carry.capacity() + s_frame.capacity() + s_tail.capacity()) * sizeof(GXCommand) +
             (s_draws.capacity() + s_carry_draws.capacity()) * sizeof(void *);
    if (s_worker_on.load(std::memory_order_relaxed))
        for (const auto &p : s_pkts)  // the render thread never resizes these
            m.cmds += p.frame.capacity() * sizeof(GXCommand) + p.draws.capacity() * sizeof(void *);
    nwii::runtime::gx::DLCacheMemStats(m.dl_n, m.dl_cmds, m.dl_b);
    return m;
}

// True on the render worker thread (renderer_gl.cpp defers arena frees).
bool GX_OnRenderWorker() { return t_on_worker; }
// R14 R5: a render worker prepares the frames (renderer_gl.cpp: the render
// thread then never makes an emit-cache entry itself).
bool GX_RenderWorkerOn() { return s_worker_on.load(std::memory_order_relaxed); }

// ---- r36r (frente R): NWII_R_COALESCE ---------------------------------------
// The dropped part of a frame (a retracted packet, or the commands up to the
// earlier frame end of a drain that found two) keeps all its register writes,
// so the state its draws left behind reaches the next frame. In the castle
// cutscene that is ~28 000 commands per dropped frame (12 000 draws a frame,
// a matrix load and a few BP writes before each), moved command by command on
// the worker and replayed by the render thread before the frame it draws.
// With no draws left, only the LAST write of each register / XF word matters,
// except around commands that use the state at that point: an EFB copy/clear
// (BP 0x52, reads the copy/clear registers), the TLUT load (BP 0x65, reads
// 0x64 and guest RAM) and 0x64 itself. Those are kept and split the dropped
// part into segments; inside a segment a BP write is dead when the same
// register is written again later in the segment (TEV colour registers
// 0xE0-0xE7: same register AND same colour/konst half, bit 23), an XF write
// when every word it writes is written again later in the segment. Dead
// writes are removed; nothing else changes (order kept), so the state at
// every kept command and at the end is the same: the same image.
// CP writes are not touched (RenderOnce ignores them anyway).
// NWII_R_COALESCE=1 on (default off). NWII_R_COALESCE_CHECK=1: simulates the
// register state of the full and of the compacted list at every kept barrier
// and at the end, and counts mismatches ([RCOAL] line, worker, every ~2 s).
// [r35m4 m5] interruptores del frente R ENCENDIDOS por defecto: NWII_R_<X>=1/0 explicito (PC) manda; si no esta,
// encendido salvo NWII_R_OFF (que si llega desde debug.toml: apply_env borra los "0"/"off").
static inline bool m5_rflag(const char *e) { return e ? e[0] == '1' : (NWII_ENV("NWII_R_OFF") == nullptr); }
namespace rcoal {
static bool On() {
    static const bool on = [] {
        const char *e = NWII_ENV("NWII_R_COALESCE");
        return m5_rflag(e);
    }();
    return on;
}
static bool CheckOn() {
    static const bool on = NWII_ENV("NWII_R_COALESCE_CHECK") != nullptr;
    return on;
}
constexpr uint32_t kXfWords = 0x1100;  // matrix memory, lights, registers 0x1000-0x10FF
static uint32_t s_xf_stamp[kXfWords];
static uint32_t s_bp_stamp[256 + 16];
static uint32_t s_stamp = 0;
static std::vector<uint8_t> s_dead;
static uint64_t st_in = 0, st_draws = 0, st_dead = 0, st_calls = 0, st_checks = 0, st_bad = 0;
static inline bool IsBarrierBP(uint8_t reg) { return reg == 0x52 || reg == 0x64 || reg == 0x65; }
static inline uint32_t BpKey(const nwii::runtime::gx::GXCommand &c) {
    const uint8_t reg = (uint8_t)c.reg;
    if (reg >= 0xE0 && reg <= 0xE7) return 256u + (uint32_t)(reg - 0xE0) * 2u + ((c.val >> 23) & 1u);
    return reg;
}
static inline void NewSegment() {
    if (++s_stamp == 0) {  // wrapped: forget every stamp
        std::memset(s_xf_stamp, 0, sizeof s_xf_stamp);
        std::memset(s_bp_stamp, 0, sizeof s_bp_stamp);
        s_stamp = 1;
    }
}
// Marks the dead writes of v[b, e) in s_dead[i - b] (draws are ignored: the
// caller drops them). Returns the number of dead writes.
static size_t Mark(const std::vector<nwii::runtime::gx::GXCommand> &v, size_t b, size_t e) {
    using nwii::runtime::gx::GXCommandType;
    s_dead.assign(e - b, 0);
    NewSegment();
    size_t dead = 0;
    for (size_t i = e; i-- > b;) {
        const auto &c = v[i];
        if (c.type == GXCommandType::BPRegister) {
            const uint8_t reg = (uint8_t)c.reg;
            if (IsBarrierBP(reg)) { NewSegment(); continue; }
            const uint32_t k = BpKey(c);
            if (s_bp_stamp[k] == s_stamp) { s_dead[i - b] = 1; ++dead; }
            else s_bp_stamp[k] = s_stamp;
        } else if (c.type == GXCommandType::XFRegister) {
            const uint32_t a = c.reg, n = (uint32_t)c.payload.size();
            if (n == 0 || a + n > kXfWords) continue;  // keep (not tracked)
            bool all = true;
            for (uint32_t k = 0; k < n; ++k) {
                if (s_xf_stamp[a + k] != s_stamp) { all = false; s_xf_stamp[a + k] = s_stamp; }
            }
            if (all) { s_dead[i - b] = 1; ++dead; }
        }
    }
    return dead;
}
// NWII_R_COALESCE_CHECK: the register state a list of commands leaves (BP
// registers, TEV colour/konst halves, XF words), compared at every barrier.
struct Sim {
    uint32_t bp[256 + 16];
    uint32_t xf[kXfWords];
    void reset() { std::memset(this, 0, sizeof *this); }
    void apply(const nwii::runtime::gx::GXCommand &c) {
        using nwii::runtime::gx::GXCommandType;
        if (c.type == GXCommandType::BPRegister) {
            bp[(uint8_t)c.reg] = c.val;
            if ((uint8_t)c.reg >= 0xE0 && (uint8_t)c.reg <= 0xE7) bp[BpKey(c)] = c.val;
        } else if (c.type == GXCommandType::XFRegister) {
            const uint32_t a = c.reg, n = (uint32_t)c.payload.size();
            for (uint32_t k = 0; k < n && a + k < kXfWords; ++k) std::memcpy(&xf[a + k], &c.payload[k], 4);
        }
    }
};
// full = the original commands (draws included or not), kept = what remains.
static void Check(const std::vector<nwii::runtime::gx::GXCommand> &full, size_t b, size_t e,
                  const std::vector<uint8_t> &dead) {
    using nwii::runtime::gx::GXCommandType;
    static Sim *sa = new Sim, *sb = new Sim;
    sa->reset();
    sb->reset();
    bool ok = true;
    for (size_t i = b; i < e; ++i) {
        const auto &c = full[i];
        if (c.type == GXCommandType::DrawPrimitive) continue;
        const bool barrier = c.type == GXCommandType::BPRegister && IsBarrierBP((uint8_t)c.reg);
        if (barrier && std::memcmp(sa, sb, sizeof(Sim))) ok = false;
        sa->apply(c);
        if (!dead[i - b]) sb->apply(c);
    }
    if (std::memcmp(sa, sb, sizeof(Sim))) ok = false;
    ++st_checks;
    if (!ok) ++st_bad;
}
// Drops the draws and the dead writes of v[b, e) in place (order kept);
// returns the new end. `on_kept(c)` runs on every kept command.
template <class F>
static size_t Compact(std::vector<nwii::runtime::gx::GXCommand> &v, size_t b, size_t e, F on_kept) {
    using nwii::runtime::gx::GXCommandType;
    const bool on = On();
    if (on) {
        st_dead += Mark(v, b, e);
        ++st_calls;
        st_in += e - b;
        if (CheckOn()) Check(v, b, e, s_dead);
    }
    size_t w = b;
    for (size_t i = b; i < e; ++i) {
        auto &c = v[i];
        if (c.type == GXCommandType::DrawPrimitive) { if (on) ++st_draws; continue; }
        if (on && s_dead[i - b]) continue;
        on_kept(c);
        if (w != i) v[w] = std::move(c);
        ++w;
    }
    return w;
}
static void Report() {
    if (!On()) return;
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(2)) return;
    last = now;
    std::printf("[RCOAL] dropped parts=%llu commands=%llu (draws %llu) dead writes removed=%llu kept=%llu"
                " | check: %llu lists, %llu mismatches\n",
                (unsigned long long)st_calls, (unsigned long long)st_in, (unsigned long long)st_draws,
                (unsigned long long)st_dead, (unsigned long long)(st_in - st_draws - st_dead),
                (unsigned long long)st_checks, (unsigned long long)st_bad);
    st_calls = st_in = st_draws = st_dead = 0;
}
}  // namespace rcoal

// r36r NWII_R_PREFIX=1: a retracted packet's kept commands go to its `pre`
// list instead of the front of the next frame, so the next frame takes the
// usual path (its commands handed over as parsed, the parser's draw entries
// kept) instead of being moved command by command and every draw prepared
// again. The renderer runs `pre` first: the same commands in the same order.
static bool RPrefixOn() {
    static const bool on = [] {
        const char *e = NWII_ENV("NWII_R_PREFIX");
        return m5_rflag(e);
    }();
    return on;
}

// A frame packet the worker took back before the render thread took it (a
// newer frame is complete): it becomes the dropped part of the next frame,
// exactly as a drain that finds two frame ends drops the first one's draws.
static void RetractToPrefix(FramePacket &pkt) {
    auto &v = pkt.frame;
    pkt.rprep.clear();  // r40d (its draws are dropped)
    pkt.merges.clear();
    // r40d NWII_D_DROPFAST: frame[0, start) is the dropped part of the packet's
    // batch (its kept writes are already in pkt.pre): never looked at again.
    const size_t b = std::min(pkt.start, v.size());
    pkt.start = 0;
    if (rcoal::On()) {
        const size_t w = rcoal::Compact(v, b, v.size(), [&](nwii::runtime::gx::GXCommand &c) {
            if (c.type == nwii::runtime::gx::GXCommandType::BPRegister && c.reg == 0x52) {
                if ((c.val & 0x4000) && !c.stale_copy) ++pkt.stats.dropped;
                c.stale_copy = true;
            }
        });
        v.erase(v.begin() + w, v.end());
        if (b) v.erase(v.begin(), v.begin() + b);  // r40d
        pkt.draws.clear();
        pkt.entries.clear();
        pkt.pack.clear();
        pkt.has_draws = false;
        ++pkt.stats.retracts;
        if (RPrefixOn()) {
            pkt.pre.insert(pkt.pre.end(), std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
            v.clear();
        }
        return;
    }
    size_t w = b;  // r40d (was 0)
    for (size_t i = b; i < v.size(); ++i) {
        auto &c = v[i];
        if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive) continue;
        if (c.type == nwii::runtime::gx::GXCommandType::BPRegister && c.reg == 0x52) {
            if ((c.val & 0x4000) && !c.stale_copy) ++pkt.stats.dropped;
            c.stale_copy = true;
        }
        if (w != i) v[w] = std::move(c);
        ++w;
    }
    v.erase(v.begin() + w, v.end());
    if (b) v.erase(v.begin(), v.begin() + b);  // r40d
    pkt.draws.clear();
    pkt.entries.clear();
    pkt.pack.clear();
    pkt.has_draws = false;
    ++pkt.stats.retracts;
    if (RPrefixOn()) {  // r36r
        pkt.pre.insert(pkt.pre.end(), std::make_move_iterator(v.begin()), std::make_move_iterator(v.end()));
        v.clear();
    }
}

// ---- r45p (frente P): NWII_P_TRIPLE ------------------------------------------
// With two packets the worker, when a new frame end is parsed and the render
// thread has not taken the waiting packet yet, TAKES IT BACK at once (acquire:
// RetractToPrefix) and refills it: from then until the new frame is handed over
// (the rest of PrepFrame: retraction with its destruction of ~30 000 commands,
// split, decode, prep: 4-10 ms in the world selector) there is NO frame for the
// render thread, which, when it is the slow side, finishes in that window and
// waits (console, selector: 2,4 ms per drawn frame "sin fotograma que dibujar").
// NWII_P_TRIPLE=1: three packets. The worker fills one that is neither drawn nor
// waiting; the waiting one stays available to the render thread until the new
// one is complete. Only then, if it is still waiting, it is retracted exactly as
// before (same RetractToPrefix rules: its kept register writes, rcoal's dead
// writes removed, EFB copies stale) and those commands go FIRST in the new
// packet's `pre` (before the new batch's own dropped part), its parser seeds
// first (the new ones override), its counters added: the render thread gets
// the same command stream as when the retraction happened at acquire. The
// retracted packet's dropped commands are destroyed after the hand-over (not in
// the window). Needs NWII_R_PREFIX and NWII_R_COALESCE (defaults); off with
// NWII_NOFRAMESKIP.
static std::vector<nwii::runtime::gx::GXCommand> s_p_junk;  // worker only
static uint64_t s_p_merges = 0, s_p_win_ns = 0, s_p_win_max = 0, s_p_pub = 0;
static bool PTripleOn() {
    static const bool on = [] {
        const char *e = NWII_ENV("NWII_P_TRIPLE");
        const bool want = e && e[0] == '1';
        const bool ok = want && RPrefixOn() && rcoal::On() && NWII_ENV("NWII_NOFRAMESKIP") == nullptr;
        if (want)
            std::printf("[P45] NWII_P_TRIPLE=1: tres paquetes, el que espera sigue disponible hasta que el "
                        "nuevo esta listo%s\n", ok ? "" : " -- NO se usa (necesita R_PREFIX y R_COALESCE, sin NOFRAMESKIP)");
        return ok;
    }();
    return on;
}
// r45p (p45b): retracting the waiting packet with s_ready taken away left the render thread
// without a frame for ~3,7 ms on the Switch (Compact over ~30 000 commands; first version). Now the waiting packet's kept
// commands are COPIED while it is still available (rcoal::Mark only reads it; the copies get
// stale_copy, as the moved ones did), and only if it is still waiting once that is done it is
// taken: then the window is the splice of three vectors. s_p_reading (under s_pm) keeps the
// render thread from freeing a packet the worker is reading (it then leaves it to the worker).
static FramePacket *s_p_reading = nullptr;
static std::vector<nwii::runtime::gx::GXCommand> s_p_kept;  // worker only
static uint64_t s_p_kept_dropped = 0;
static void CopyKept(const FramePacket &pkt) {
    using nwii::runtime::gx::GXCommand;
    using nwii::runtime::gx::GXCommandType;
    const auto &v = pkt.frame;
    const size_t b = std::min(pkt.start, v.size()), e = v.size();
    s_p_kept.clear();
    s_p_kept_dropped = 0;
    rcoal::st_dead += rcoal::Mark(v, b, e);
    ++rcoal::st_calls;
    rcoal::st_in += e - b;
    for (size_t i = b; i < e; ++i) {
        const GXCommand &c = v[i];
        if (c.type == GXCommandType::DrawPrimitive) { ++rcoal::st_draws; continue; }
        if (rcoal::s_dead[i - b]) continue;
        s_p_kept.push_back(c);
        if (c.type == GXCommandType::BPRegister && c.reg == 0x52) {
            if ((c.val & 0x4000) && !c.stale_copy) ++s_p_kept_dropped;
            s_p_kept.back().stale_copy = true;
        }
    }
}
// r45p NWII_P_KEEPCOPY=1 (with NWII_P_TRIPLE): a waiting frame that holds an EFB copy to a texture
// whose destination (BP 0x4B) the newer frame does not copy to again is NOT dropped: retracted, that
// copy would only be cleared (stale) and the texture never made (the world selector's background:
// one copy of the map, 640x456, when «−» is pressed; dropped, the background stays black). The worker
// waits until the render thread takes it (at most ~100 ms) and hands the new one over after it.
static bool PKeepCopyOn() {
    static const bool on = [] {
        const char *e = NWII_ENV("NWII_P_KEEPCOPY");
        const bool v = e && e[0] == '1';
        if (v) std::printf("[P45] NWII_P_KEEPCOPY=1: no se descarta un fotograma con una copia EFB que el siguiente no repite\n");
        return v;
    }();
    return on;
}
static uint64_t s_p_kept_frames = 0, s_p_keep_wait_ns = 0;
// The copy destinations (BP 0x4B value, 0xFFFFFFFF if the list set none before the copy) of the
// texture copies (not XFB, not stale) in v[b, e).
static void CopyDests(const std::vector<nwii::runtime::gx::GXCommand> &v, size_t b, std::vector<uint32_t> &out) {
    using nwii::runtime::gx::GXCommandType;
    uint32_t dest = 0xFFFFFFFFu;
    for (size_t i = b; i < v.size(); ++i) {
        const auto &c = v[i];
        if (c.type != GXCommandType::BPRegister) continue;
        if (c.reg == 0x4B) dest = c.val & 0xFFFFFF;
        else if (c.reg == 0x52 && !(c.val & 0x4000) && !c.stale_copy) out.push_back(dest);
    }
}
static bool HasLoneCopy(const FramePacket &old, const FramePacket &n) {
    static std::vector<uint32_t> a, b;
    a.clear();
    b.clear();
    CopyDests(old.frame, std::min(old.start, old.frame.size()), a);
    if (a.empty()) return false;
    CopyDests(n.pre, 0, b);
    CopyDests(n.frame, std::min(n.start, n.frame.size()), b);
    for (uint32_t d : a) {
        if (d == 0xFFFFFFFFu) return true;
        if (std::find(b.begin(), b.end(), d) == b.end()) return true;
    }
    return false;
}
// `old` was waiting and is now taken (s_ready cleared); its kept commands are in s_p_kept (CopyKept).
static void SpliceRetracted(FramePacket &old, FramePacket &n) {
    std::vector<nwii::runtime::gx::GXCommand> fin;
    fin.swap(old.pre);
    fin.reserve(fin.size() + s_p_kept.size() + n.pre.size());
    fin.insert(fin.end(), std::make_move_iterator(s_p_kept.begin()), std::make_move_iterator(s_p_kept.end()));
    fin.insert(fin.end(), std::make_move_iterator(n.pre.begin()), std::make_move_iterator(n.pre.end()));
    n.pre.swap(fin);
    s_p_junk.swap(fin);  // the moved-from shells, freed after the hand-over
    s_p_kept.clear();
    old.rprep.clear();
    old.merges.clear();
    old.draws.clear();
    old.entries.clear();
    old.pack.clear();
    old.has_draws = false;
    old.stats.dropped += s_p_kept_dropped;
    ++old.stats.retracts;
    // parser seeds (the retracted frame first, the new one overrides; as PrepFrame) and counters
    auto &ns = n.state;
    const auto &os = old.state;
    for (int w = 0; w < 4; ++w) {
        for (uint64_t m = os.seed_mask[w] & ~ns.seed_mask[w]; m; m &= m - 1) {
            const int r = w * 64 + __builtin_ctzll(m);
            ns.seed_bp[r] = os.seed_bp[r];
        }
        ns.seed_mask[w] |= os.seed_mask[w];
    }
    for (int k = 0; k < 8; ++k) {
        if ((os.seed_kmask & ~ns.seed_kmask) & (1u << k))
            ns.seed_konst[k / 2][k & 1] = os.seed_konst[k / 2][k & 1];
        if ((os.seed_cmask & ~ns.seed_cmask) & (1u << k))
            ns.seed_color[k / 2][k & 1] = os.seed_color[k / 2][k & 1];
    }
    ns.seed_kmask |= os.seed_kmask;
    ns.seed_cmask |= os.seed_cmask;
    PrepStats st = old.stats;
    st.add(n.stats);
    n.stats = st;
    old.stats = PrepStats{};
}

// ---- r40d (frente D): NWII_D_DROPFAST ----------------------------------------
// A drain whose batch holds two or more frame ends (the worker fell behind)
// drops the frames before the last one. The old path (PrepFrame, `else` below)
// moves EVERY command of the batch one by one into s_frame, the kept frame's
// included, and forgets the parser's draw entries (all its draws prepared
// again): on the Switch ~14 ms split + ~8 ms decode per prepared frame in
// agua_pinguino and the castle party, so the next batch holds two or three
// frame ends again (the slow regime keeps itself going).
// NWII_D_DROPFAST=1: the dropped part [0, D) of the batch is handled as a
// retracted packet is (RetractToPrefix): its kept register writes (no draws,
// rcoal's dead writes removed, EFB copies stale) are APPENDED to pkt.pre, which
// the renderer runs first, in the same order; the kept frame takes the fast path
// (s_carry handed over whole, the parser's entries kept) and the renderer starts
// at frame[D] (FramePacket::start, g_render_start). The dropped commands stay in
// the frame vector, never rendered, until the packet is freed (render thread or
// worker, as the frame); their owning pointers (DrawRaw, decoded vertices: counts
// not atomic, shared with the display-list cache) are reset here, on the worker,
// so the packet's render_frees still holds. One backward pass over [0, D) does
// rcoal's marking (same rules as rcoal::Mark) and collects the kept commands;
// only those (~15 % of the dropped part) are moved.
static uint64_t s_d_stops = 0;  // r40d NWII_D_PARSESTOP: parses stopped at a frame end
static uint64_t s_d_counted = 0, s_d_seen = 0, s_d_left_max = 0;  // r40d: ends counted by the guest / noted by the parser
// r40d NWII_D_LIGHTPARSE=1 (needs NWII_D_PARSESTOP and NWII_D_DROPFAST): when the
// taken bytes hold P >= 2 frame ends, the first P-1 frames are dropped anyway
// (only the last one is drawn): the parser does not write their draws (see
// FifoParser::SetDoomedXfb), so a dropped frame costs its register commands only.
// If fewer ends than expected are parsed (a miscount: should not happen, the guest
// counts each XFB copy as it queues it), the frames parsed light so far are all
// dropped, including the last one parsed (no frame handed over this time; their
// kept writes wait in s_d_pre_carry for the next packet), and the light parse
// goes on until enough ends are dropped: a frame without its draws is never drawn.
// NWII_D_LIGHTPARSE_CHECK=1 (tests): full parse, counts how often the prediction
// would have failed.
static bool LightParseOn() {
    static const bool on = [] {
        const char *e = NWII_ENV("NWII_D_LIGHTPARSE");
        const bool v = e && e[0] && e[0] != '0';
        if (v) std::printf("[D40] parse ligero de los fotogramas que se tiran: NWII_D_LIGHTPARSE=1\n");
        return v;
    }();
    return on;
}
static bool LightCheckOn() {
    static const bool on = NWII_ENV("NWII_D_LIGHTPARSE_CHECK") != nullptr;
    return on;
}
static uint32_t s_d_doom_left = 0;
static std::vector<nwii::runtime::gx::GXCommand> s_d_pre_carry;
static uint64_t s_d_light_calls = 0, s_d_light_fail = 0, s_d_light_noframe = 0;
namespace dfast {
static bool On() {
    static const bool on = [] {
        const char *e = NWII_ENV("NWII_D_DROPFAST");
        const bool v = e && e[0] && e[0] != '0';
        if (v) std::printf("[D40] descarte barato en el trabajador: NWII_D_DROPFAST=1\n");
        return v;
    }();
    return on;
}
// Counters (worker), printed by Report every ~2 s when there was a multi-end
// batch: fast path (this) and old path (PrepFrame's `else`), for both settings.
static uint64_t st_parts = 0, st_frames = 0, st_cmds = 0, st_kept = 0, st_ticks = 0;
static uint64_t st_old_parts = 0, st_old_frames = 0, st_old_cmds = 0, st_old_ticks = 0;
static std::vector<uint32_t> s_keep;
// The dropped part v[0, D): kept writes appended to `pre`, draws disowned.
static void DropPart(std::vector<nwii::runtime::gx::GXCommand> &v, size_t D,
                     std::vector<nwii::runtime::gx::GXCommand> &pre, uint64_t &dropped_xfb) {
    using nwii::runtime::gx::GXCommandType;
    const uint64_t t0 = wprof::Tick();
    const bool coal = rcoal::On(), check = coal && rcoal::CheckOn();
    if (check) rcoal::s_dead.assign(D, 0);
    s_keep.clear();
    rcoal::NewSegment();
    size_t draws = 0, dead = 0;
    for (size_t i = D; i-- > 0;) {
        auto &c = v[i];
        if (i >= 8) __builtin_prefetch(&v[i - 8]);
        if (c.type == GXCommandType::DrawPrimitive) {
            ++draws;
            if (c.raw) c.raw.reset();
            if (c.shared_vertices) c.shared_vertices.reset();
            continue;
        }
        if (coal) {
            if (c.type == GXCommandType::BPRegister) {
                const uint8_t reg = (uint8_t)c.reg;
                if (rcoal::IsBarrierBP(reg)) {
                    rcoal::NewSegment();
                } else {
                    const uint32_t k = rcoal::BpKey(c);
                    if (rcoal::s_bp_stamp[k] == rcoal::s_stamp) {
                        ++dead;
                        if (check) rcoal::s_dead[i] = 1;
                        continue;
                    }
                    rcoal::s_bp_stamp[k] = rcoal::s_stamp;
                }
            } else if (c.type == GXCommandType::XFRegister) {
                const uint32_t a = c.reg, n = (uint32_t)c.payload.size();
                if (n != 0 && a + n <= rcoal::kXfWords) {
                    bool all = true;
                    for (uint32_t k = 0; k < n; ++k)
                        if (rcoal::s_xf_stamp[a + k] != rcoal::s_stamp) {
                            all = false;
                            rcoal::s_xf_stamp[a + k] = rcoal::s_stamp;
                        }
                    if (all) {
                        ++dead;
                        if (check) rcoal::s_dead[i] = 1;
                        continue;
                    }
                }
            }
        }
        s_keep.push_back((uint32_t)i);
    }
    if (coal) {
        rcoal::st_dead += dead;
        rcoal::st_draws += draws;
        ++rcoal::st_calls;
        rcoal::st_in += D;
        if (check) rcoal::Check(v, 0, D, rcoal::s_dead);
    }
    uint64_t ends = 0;
    pre.reserve(pre.size() + s_keep.size());
    for (size_t j = s_keep.size(); j-- > 0;) {
        auto &c = v[s_keep[j]];
        if (c.type == GXCommandType::BPRegister && c.reg == 0x52) {
            if ((c.val & 0x4000) && !c.stale_copy) ++ends;
            c.stale_copy = true;
        }
        pre.push_back(std::move(c));
    }
    dropped_xfb += ends;
    ++st_parts;
    st_frames += ends;
    st_cmds += D;
    st_kept += s_keep.size();
    st_ticks += wprof::Tick() - t0;
}
// NWII_D_DROPFAST_CHECK=1 (tests): the command list the renderer gets from the
// fast drop (the appended part of pkt.pre, then frame[start, end]) must equal,
// command by command, the one the old path would have built (same commands,
// same values and payloads, same draw objects, same stale flags).
static bool CheckOn() {
    static const bool on = NWII_ENV("NWII_D_DROPFAST_CHECK") != nullptr;
    return on;
}
static uint64_t st_chk = 0, st_chk_bad = 0, st_chk_cmds = 0;
static std::vector<uint64_t> s_exp;
static inline uint64_t CmdHash(const nwii::runtime::gx::GXCommand &c, bool stale) {
    uint64_t h = (uint64_t)c.type | ((uint64_t)c.prim_type << 8) | ((uint64_t)stale << 16) |
                 ((uint64_t)c.reg << 24) | ((uint64_t)c.length << 40);
    h = h * 0x9E3779B97F4A7C15ull ^ c.val;
    h = h * 0x9E3779B97F4A7C15ull ^ c.bp_raw;
    h = h * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)c.raw.get();
    h = h * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)c.shared_vertices.get();
    h = h * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uintptr_t)c.vertices.size();
    const size_t n = c.payload.size();
    h = h * 0x9E3779B97F4A7C15ull ^ n;
    for (size_t k = 0; k < n; ++k) {
        uint32_t u;
        std::memcpy(&u, &c.payload.data()[k], 4);
        h = h * 0x100000001B3ull ^ u;
    }
    return h;
}
// Before DropPart: what the old path would hand the renderer from v[0, end].
static void CheckExpect(const std::vector<nwii::runtime::gx::GXCommand> &v, size_t D, size_t end) {
    using nwii::runtime::gx::GXCommandType;
    const bool coal = rcoal::On();
    if (coal) rcoal::Mark(v, 0, D);
    s_exp.clear();
    for (size_t i = 0; i < end; ++i) {
        const auto &c = v[i];
        if (i < D) {
            if (c.type == GXCommandType::DrawPrimitive) continue;
            if (coal && rcoal::s_dead[i]) continue;
            s_exp.push_back(CmdHash(c, c.stale_copy || (c.type == GXCommandType::BPRegister && c.reg == 0x52)));
        } else {
            s_exp.push_back(CmdHash(c, c.stale_copy));
        }
    }
}
static void CheckGot(const std::vector<nwii::runtime::gx::GXCommand> &pre, size_t pre0,
                     const std::vector<nwii::runtime::gx::GXCommand> &frame, size_t start, size_t end) {
    size_t k = 0;
    bool ok = (pre.size() - pre0) + (end - start) == s_exp.size();
    for (size_t i = pre0; ok && i < pre.size(); ++i) ok = CmdHash(pre[i], pre[i].stale_copy) == s_exp[k++];
    for (size_t i = start; ok && i < end; ++i) ok = CmdHash(frame[i], frame[i].stale_copy) == s_exp[k++];
    ++st_chk;
    st_chk_cmds += s_exp.size();
    if (!ok) ++st_chk_bad;
}
static void Report() {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(2)) return;
    last = now;
    if (CheckOn())
        std::printf("[D40CHECK] lotes comprobados=%llu ordenes=%llu distintos=%llu\n", (unsigned long long)st_chk,
                    (unsigned long long)st_chk_cmds, (unsigned long long)st_chk_bad);
    if (s_d_stops || s_d_counted != s_d_seen) {
        std::printf("[D40] parses detenidos en un final de fotograma: %llu | finales contados por el juego %llu,"
                    " vistos por el parser %llu, resto sin parsear max %llu B\n", (unsigned long long)s_d_stops,
                    (unsigned long long)s_d_counted, (unsigned long long)s_d_seen, (unsigned long long)s_d_left_max);
        s_d_stops = 0;
    }
    s_d_left_max = 0;
    if (s_d_light_calls || s_d_light_fail) {
        static uint64_t sk0 = 0;
        const uint64_t sk = nwii::runtime::gx::FifoParser::LightSkipped();
        static uint64_t ln0 = 0, lf0 = 0;
        uint64_t ln = 0, lf = 0;
        nwii::runtime::gx::FifoParser::LightTime(ln, lf);
        std::printf("[D40L] parse ligero: %.0f us por fotograma condenado (%llu)\n",
                    lf > lf0 ? (ln - ln0) / 1000.0 / (double)(lf - lf0) : 0.0, (unsigned long long)(lf - lf0));
        ln0 = ln; lf0 = lf;
        std::printf("[D40L] parses con fotogramas condenados %llu, draws no escritos %llu, prediccion fallida %llu"
                    " (sin entrega %llu)%s\n", (unsigned long long)s_d_light_calls, (unsigned long long)(sk - sk0),
                    (unsigned long long)s_d_light_fail, (unsigned long long)s_d_light_noframe,
                    LightParseOn() ? "" : " [solo comprobacion]");
        sk0 = sk;
        s_d_light_calls = s_d_light_fail = s_d_light_noframe = 0;
    }
    if (st_parts || st_old_parts)
        std::printf("[D40] lotes con varios finales: barato %llu (fot. tirados %llu, ordenes %llu, conservadas %llu,"
                    " %.0f us/lote) | viejo %llu (fot. tirados %llu, ordenes %llu, %.0f us/lote)\n",
                    (unsigned long long)st_parts, (unsigned long long)st_frames, (unsigned long long)st_cmds,
                    (unsigned long long)st_kept, st_parts ? wprof::TickUs(st_ticks) / (double)st_parts : 0.0,
                    (unsigned long long)st_old_parts, (unsigned long long)st_old_frames,
                    (unsigned long long)st_old_cmds,
                    st_old_parts ? wprof::TickUs(st_old_ticks) / (double)st_old_parts : 0.0);
    st_parts = st_frames = st_cmds = st_kept = st_ticks = 0;
    st_old_parts = st_old_frames = st_old_cmds = st_old_ticks = 0;
}
}  // namespace dfast

// r40d NWII_D_PARSESTOP=1 (worker only): the parse stops right after the last
// frame end the taken bytes hold (the guest counts them as it queues them,
// s_fifo_frames); what follows (the next frame's start, often most of it when
// the worker is late: 13-24 k commands in agua_pinguino) stays unparsed for the
// next call instead of being parsed now and then moved out of the frame (the
// "tail", one 128-byte move per command). The same commands reach the renderer
// in the same order; only the parser state taken with the packet (CP/VAT/
// arrays, BP seeds) is the one at the frame end instead of some way into the
// next frame (as when no tail was parsed, which already happens).
static bool ParseStopOn() {
    static const bool on = [] {
        const char *e = NWII_ENV("NWII_D_PARSESTOP");
        const bool v = e && e[0] && e[0] != '0';
        if (v) std::printf("[D40] parse hasta el ultimo final de fotograma: NWII_D_PARSESTOP=1\n");
        return v;
    }();
    return on;
}
static uint32_t s_d_pend = 0;  // frame ends taken from the FIFO and not parsed yet (worker)
static std::vector<uint8_t> s_d_spare;  // r40d: the guest's FIFO bytes taken by swap (worker)

// The preparing half of a drain (see above). Returns the packet a frame was
// produced into (nullptr: none yet): the one `acquire` returns (called once a frame end is
// parsed; `retracted`: it hands back a packet not rendered yet, whose
// commands become the dropped start of this frame). apply_now: the render
// thread is the caller (no worker): the parser state goes to g_state right
// after each parse, as the single-threaded drain always did.
static FramePacket *PrepFrame(FramePacket *(*acquire)(bool &retracted), bool apply_now) {
    if (PhOn()) s_ph_t = PhNow();
    wprof::Stage st;  // R22 R7
    if (s_host_tid.load(std::memory_order_relaxed) != std::this_thread::get_id())
        s_host_tid.store(std::this_thread::get_id(), std::memory_order_relaxed);
    TrimBuffers();
    {
        std::lock_guard<FifoLock> lock(g_fifo_mutex);
        // r38c NWII_C_STRESS_FIFO_US (solo pruebas): el trabajador retiene el candado del FIFO ese tiempo
        // (girando) en cada llamada, para agrandar la ventana del cuelgue de Z22 y reproducirlo.
        static const long s_c38_stress = [] { const char *e = NWII_ENV("NWII_C_STRESS_FIFO_US"); return e ? std::atol(e) : 0L; }();
        if (__builtin_expect(s_c38_stress > 0, 0) && !apply_now) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(s_c38_stress);
            while (std::chrono::steady_clock::now() < until) {}
        }
        if (!apply_now) {  // worker: still not taken by the render thread
            s_held_frames += s_fifo_frames;
            s_prep_pending.held += s_fifo_frames;
            s_d_pend += s_fifo_frames;  // r40d
            s_d_counted += s_fifo_frames;
        }
        s_fifo_frames = 0;  // every queued byte is taken below
        if (s_parse_buf.empty()) {
            // Nothing carried over: take the guest's buffer as is and leave
            // ours (empty, capacity kept) for it to fill.
            s_parse_buf.swap(g_hw_fifo);
        } else if (!g_hw_fifo.empty()) {
            if (ParseStopOn() && !apply_now) {
                // r40d: NWII_D_PARSESTOP leaves bytes in s_parse_buf on almost every
                // call, so the copy below would run under g_fifo_mutex every time
                // (the m5/Z22 priority inversion: the worker preempted on core 2
                // holding this lock while the game spins for it; D06 hung like that).
                // Take the guest's buffer with a swap (O(1)) and append it after
                // unlocking; the guest gets our empty spare (capacity kept).
                s_d_spare.swap(g_hw_fifo);
            } else {
                s_parse_buf.insert(s_parse_buf.end(), g_hw_fifo.begin(),
                                   g_hw_fifo.end());
                g_hw_fifo.clear();
            }
        }
        // R8 memory: the guest's (now empty) buffer, see TrimBuffers. Rare:
        // only after a backlog.
        if (g_hw_fifo.capacity() > 4 * std::max<size_t>(s_hw_bytes.get(), 1u << 20))
            TrimVec(g_hw_fifo, s_hw_bytes.get());
    }
    if (!s_d_spare.empty()) {  // r40d (see above): outside the lock
        s_parse_buf.insert(s_parse_buf.end(), s_d_spare.begin(), s_d_spare.end());
        s_d_spare.clear();
    }
    s_hw_bytes.note(s_parse_buf.size(), std::chrono::steady_clock::now());
    st.mark(wprof::kLock);
    if (s_parse_buf.empty()) return nullptr;

    const uint64_t cpu0 = thread_cpu_us();
    struct PrepCpu {
        uint64_t c0;
        bool armed = true;
        ~PrepCpu() { if (armed) s_prep_pending.prep_cpu_us += thread_cpu_us() - c0; }
    } prep_cpu{cpu0};
    auto t0 = std::chrono::steady_clock::now();
    // R7 perf: parse straight onto the commands carried over from earlier
    // drains (a frame usually arrives over several 1 ms polls): parsing into
    // a separate vector and moving it onto s_carry cost a pass over the
    // frame so far per poll (~26k commands, ~2.7 MB on the world map) and,
    // with the exact reserve in ExpandDisplayList, grew quadratically when
    // the host fell behind. s_carry never holds a frame end
    // between calls (see below), so only the commands parsed now need to be
    // scanned for one.
    const size_t carried = s_carry.size();
    PH(7);
    const bool use_idx = PIndexOn();
    nwii::runtime::gx::FifoParser::SetFrameSeq(s_prep_frames + 1);
    const bool pstop = ParseStopOn() && use_idx && !apply_now;  // r40d
    nwii::runtime::gx::FifoParser::SetStopAfterXfb(pstop ? s_d_pend : 0);
    // r40d NWII_D_LIGHTPARSE: frame ends whose frames are known to be dropped.
    const bool light = pstop && dfast::On() && RPrefixOn() && (LightParseOn() || LightCheckOn());
    uint32_t doom = 0;
    if (light) {
        doom = std::max<uint32_t>(s_d_doom_left, s_d_pend >= 2 ? s_d_pend - 1 : 0);
        if (doom) ++s_d_light_calls;
        nwii::runtime::gx::FifoParser::SetDoomedXfb(LightParseOn() ? doom : 0);
    }
    nwii::runtime::gx::FifoParser::Parse(s_parse_buf, s_carry, use_idx ? &s_pindex : nullptr);
    bool doom_fail = false;
    if (light) {
        nwii::runtime::gx::FifoParser::SetDoomedXfb(0);
        const uint32_t ends = (uint32_t)s_pindex.xfb_copies.size();
        if (doom && ends <= doom) {
            ++s_d_light_fail;
            doom_fail = LightParseOn();
            s_d_doom_left = doom_fail ? doom - ends : 0;
        } else {
            s_d_doom_left = 0;
        }
    }
    if (!apply_now && use_idx) {  // r40d: guest count vs parser (diagnostic, [D40])
        s_d_seen += s_pindex.xfb_copies.size();
        s_d_left_max = std::max<uint64_t>(s_d_left_max, s_parse_buf.size());
    }
    if (pstop) {
        const uint32_t ends = (uint32_t)s_pindex.xfb_copies.size();
        const bool stopped = nwii::runtime::gx::FifoParser::StoppedAtXfb();
        if (stopped) ++s_d_stops;
        // all bytes parsed: nothing counted is left (a miscount does not linger)
        s_d_pend = (!stopped || ends >= s_d_pend) ? 0 : s_d_pend - ends;
        nwii::runtime::gx::FifoParser::SetStopAfterXfb(0);
    }
    if (apply_now) {
        // Same thread renders: the parser's register state reaches g_state
        // at the end of every parse, as when the parser wrote it directly.
        static nwii::runtime::gx::ParseRenderState *st = new nwii::runtime::gx::ParseRenderState;
        nwii::runtime::gx::FifoParser::TakeRenderState(*st);
        nwii::runtime::gx::FifoParser::ApplyRenderState(*st);
    }
    PH(0);
    st.mark(wprof::kParse);
    auto t1 = std::chrono::steady_clock::now();
    s_prep_pending.parse_us +=
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    static const bool s_sample = NWII_ENV("NWII_SAMPLE") != nullptr;
    if (s_sample && !s_parse_buf.empty()) {
        static size_t last_size = 0;
        static int stuck_ticks = 0, dumps = 0;
        if (s_parse_buf.size() == last_size) {
            if (++stuck_ticks == 60 && dumps < 8) {
                dumps++;
                stuck_ticks = 0;
                std::cout << "[GX] stalled remainder " << std::dec
                          << s_parse_buf.size() << " bytes, head:" << std::hex;
                for (size_t i = 0; i < s_parse_buf.size() && i < 24; i++)
                    std::cout << " " << (unsigned)s_parse_buf[i];
                std::cout << std::dec << "\n";
            }
        } else {
            last_size = s_parse_buf.size();
            stuck_ticks = 0;
        }
    }

    if (s_parse_buf.size() > (4u << 20))
        s_parse_buf.clear();

    PH(1);
    int last_frame_end = -1;
    int prev_frame_end = -1;
    if (use_idx) {
        // R22 R7: noted by the parser while it wrote them (no pass over the
        // just-written commands, ~3.7 MB per frame in the castle cutscene).
        static const bool s_pcheck = NWII_ENV("NWII_PINDEX_CHECK") != nullptr;
        if (s_pcheck) {
            // Test: the index must equal what the old scan finds.
            static uint64_t n = 0, bad = 0;
            size_t kd = 0, kx = 0;
            bool ok = true;
            for (size_t i = carried; i < s_carry.size(); ++i) {
                const auto &c = s_carry[i];
                if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive)
                    ok = ok && kd < s_pindex.draws.size() && s_pindex.draws[kd++] == i;
                if (c.type == nwii::runtime::gx::GXCommandType::BPRegister && c.reg == 0x52 && (c.val & 0x4000))
                    ok = ok && kx < s_pindex.xfb_copies.size() && s_pindex.xfb_copies[kx++] == i;
            }
            ok = ok && kd == s_pindex.draws.size() && kx == s_pindex.xfb_copies.size() &&
                 s_pindex.ents.size() == s_pindex.draws.size();
            for (size_t j = 0; ok && j < s_pindex.draws.size(); ++j) {
                const auto &c = s_carry[s_pindex.draws[j]];
                const void *e = s_pindex.ents[j];
                if (e != nwii::runtime::gx::FifoParser::kEntryUnknown && c.shared_vertices &&
                    e != c.shared_vertices->emit_cache.get())
                    ok = false;
            }
            ++n;
            if (!ok) ++bad;
            if ((!ok && bad < 20) || n % 2000 == 0)
                std::printf("[PINDEX_CHECK] parses=%llu mismatches=%llu\n", (unsigned long long)n,
                            (unsigned long long)bad);
        }
        s_carry_draws.insert(s_carry_draws.end(), s_pindex.draws.begin(), s_pindex.draws.end());
        s_carry_ents.insert(s_carry_ents.end(), s_pindex.ents.begin(), s_pindex.ents.end());
        const auto &xc = s_pindex.xfb_copies;
        if (!xc.empty()) {
            last_frame_end = (int)xc.back();
            if (xc.size() >= 2) prev_frame_end = (int)xc[xc.size() - 2];
        }
        s_pindex.clear();
    } else {
    for (size_t i = carried; i < s_carry.size(); ++i) {
        const auto &c = s_carry[i];
        if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive) {
            s_carry_draws.push_back((uint32_t)i);
            s_carry_ents.push_back(nwii::runtime::gx::FifoParser::kEntryUnknown);
        }
        if (c.type == nwii::runtime::gx::GXCommandType::BPRegister &&
            c.reg == 0x52 && (c.val & 0x4000)) {
            prev_frame_end = last_frame_end;
            last_frame_end = (int)i;
        }
    }
    }
    PH(2);
    st.mark(wprof::kScan);
    if (doom_fail && last_frame_end >= 0) {
        // r40d NWII_D_LIGHTPARSE miscount: every frame parsed up to the last end
        // is dropped (some lack their draws); nothing is handed over.
        const size_t E = (size_t)last_frame_end + 1;
        dfast::DropPart(s_carry, E, s_d_pre_carry, s_prep_pending.dropped);
        s_carry.erase(s_carry.begin(), s_carry.begin() + (ptrdiff_t)E);
        size_t k = 0, w = 0;
        while (k < s_carry_draws.size() && s_carry_draws[k] < E) ++k;
        for (; k < s_carry_draws.size(); ++k) {
            s_carry_ents[w] = s_carry_ents[k];
            s_carry_draws[w++] = s_carry_draws[k] - (uint32_t)E;
        }
        s_carry_draws.resize(w);
        s_carry_ents.resize(w);
        ++s_d_light_noframe;
        return nullptr;
    }
    if (last_frame_end < 0)
        return nullptr;
    bool retracted = false;
    FramePacket &pkt = *acquire(retracted);
    static nwii::runtime::gx::ParseRenderState *s_prev_state = new nwii::runtime::gx::ParseRenderState;
    PrepStats prev_stats;
    if (retracted) {
        RetractToPrefix(pkt);
        *s_prev_state = pkt.state;
        prev_stats = pkt.stats;
    } else if (!pkt.frame.empty() || !pkt.draws.empty()) {
        // The render thread is done with this packet's last frame: its
        // commands die here (worker: on the worker thread).
        pkt.frame.clear();
        pkt.draws.clear();
        pkt.entries.clear();
        pkt.pack.clear();
    }
    if (!retracted) pkt.pre.clear();  // r36r NWII_R_PREFIX: already rendered
    if (!s_d_pre_carry.empty()) {  // r40d: frames dropped by a light-parse miscount (older than this batch)
        pkt.pre.insert(pkt.pre.end(), std::make_move_iterator(s_d_pre_carry.begin()),
                       std::make_move_iterator(s_d_pre_carry.end()));
        s_d_pre_carry.clear();
    }
    // r36r NWII_R_PREFIX: a retracted packet's commands are in pkt.pre: the
    // new frame takes the usual path below.
    const bool retr_slow = retracted && !RPrefixOn();
    pkt.has_draws = false;
    // NWII_NOFRAMESKIP=1: render every frame of the batch (only the last one
    // is presented), so EFB copies of earlier frames are made with their draws.
    static const bool s_noskip = NWII_ENV("NWII_NOFRAMESKIP") != nullptr;
    if (s_noskip) prev_frame_end = -1;
    st.mark(wprof::kClear);

    // NWII_FRAMELOG=1: per drained batch, how many frame ends (XFB copies)
    // it held and how many draws frame-skip dropped.
    static const bool framelog = NWII_ENV("NWII_FRAMELOG") != nullptr;
    if (framelog) {
        int ends = 0, draws = 0, dropped = 0, tex = 0;
        for (int i = 0; i <= last_frame_end; ++i) {
            const auto &c = s_carry[i];
            if (c.type == nwii::runtime::gx::GXCommandType::BPRegister && c.reg == 0x52)
                ((c.val & 0x4000) ? ends : tex)++;
            if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive) {
                ++draws;
                if (i <= prev_frame_end) ++dropped;
            }
        }
        printf("[FRAMELOG] f=%llu xfb_copies=%d tex_copies=%d draws=%d dropped=%d\n",
               (unsigned long long)s_prep_frames, ends, tex, draws, dropped);
    }
    auto &frame = s_frame;
    frame.clear();
    s_draws.clear();
    // r40d NWII_D_DROPFAST: a batch with two or more frame ends also takes the
    // fast path; its dropped part [0, fstart) goes to pkt.pre (see dfast::).
    const bool dfast_now = prev_frame_end >= 0 && !retr_slow && RPrefixOn() && dfast::On();
    const size_t fstart = dfast_now ? (size_t)prev_frame_end + 1 : 0;
    if ((prev_frame_end < 0 || dfast_now) && !retr_slow) {
        const size_t d_pre0 = pkt.pre.size();
        if (dfast_now && dfast::CheckOn()) dfast::CheckExpect(s_carry, fstart, (size_t)last_frame_end + 1);
        if (dfast_now) dfast::DropPart(s_carry, fstart, pkt.pre, s_prep_pending.dropped);
        // Nothing to drop (the usual case at full speed): move only the
        // commands after the frame end (the next frame's start) out, and
        // take the rest as the frame without touching it.
        auto &tail = s_tail;
        tail.clear();
        tail.insert(tail.end(),
                    std::make_move_iterator(s_carry.begin() + last_frame_end + 1),
                    std::make_move_iterator(s_carry.end()));
        s_carry.erase(s_carry.begin() + last_frame_end + 1, s_carry.end());
        frame.swap(s_carry);
        s_carry.swap(tail);
        if (dfast_now && dfast::CheckOn()) dfast::CheckGot(pkt.pre, d_pre0, frame, fstart, frame.size());
        // Draw positions: the frame keeps those up to its end (same
        // indices), the carried-over tail gets the rest, rebased.
        size_t k = 0;
        s_draw_entries.clear();
        while (k < s_carry_draws.size() && s_carry_draws[k] < fstart) ++k;  // r40d: dropped draws
        for (; k < s_carry_draws.size() && (int)s_carry_draws[k] <= last_frame_end; ++k) {
            s_draws.push_back(&frame[s_carry_draws[k]]);
            s_draw_entries.push_back(s_carry_ents[k]);
        }
        size_t w = 0;
        for (; k < s_carry_draws.size(); ++k) {
            s_carry_ents[w] = s_carry_ents[k];
            s_carry_draws[w++] = s_carry_draws[k] - (uint32_t)(last_frame_end + 1);
        }
        s_carry_draws.resize(w);
        s_carry_ents.resize(w);
    } else {
    const uint64_t old_t0 = wprof::Tick();  // r40d: cost of this path ([D40])
    const uint64_t old_drop0 = s_prep_pending.dropped;
    frame.reserve(pkt.frame.size() + s_carry.size());
    // R11: a retracted packet's commands (draws already dropped) come first.
    for (auto &c : pkt.frame) frame.push_back(std::move(c));
    pkt.frame.clear();
    // r36r NWII_R_COALESCE: the dead writes of the dropped part [0, prev_frame_end].
    const bool coal = rcoal::On() && prev_frame_end >= 0;
    if (coal) {
        rcoal::st_dead += rcoal::Mark(s_carry, 0, (size_t)prev_frame_end + 1);
        ++rcoal::st_calls;
        rcoal::st_in += (size_t)prev_frame_end + 1;
        if (rcoal::CheckOn()) rcoal::Check(s_carry, 0, (size_t)prev_frame_end + 1, rcoal::s_dead);
    }
    for (int i = 0; i <= last_frame_end; ++i) {
        auto &c = s_carry[i];
        if (i <= prev_frame_end &&
            c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive) {
            if (coal) ++rcoal::st_draws;
            continue;
        }
        if (coal && i <= prev_frame_end && rcoal::s_dead[(size_t)i]) continue;
        // EFB copies of the dropped frames would capture an EFB without their
        // draws (one-shot render-to-texture would keep that garbage); only
        // their clears run.
        if (i <= prev_frame_end &&
            c.type == nwii::runtime::gx::GXCommandType::BPRegister &&
            c.reg == 0x52) {
            c.stale_copy = true;
            if (c.val & 0x4000) ++s_prep_pending.dropped;
        }
        frame.push_back(std::move(c));
    }
    s_carry.erase(s_carry.begin(), s_carry.begin() + last_frame_end + 1);
    for (auto &c : frame)
        if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive)
            s_draws.push_back(&c);
    // (R22 R7: rare path; entries made below, as before)
    s_draw_entries.assign(s_draws.size(), nwii::runtime::gx::FifoParser::kEntryUnknown);
    s_carry_draws.clear();
    s_carry_ents.clear();
    for (size_t i = 0; i < s_carry.size(); ++i)
        if (s_carry[i].type == nwii::runtime::gx::GXCommandType::DrawPrimitive) {
            s_carry_draws.push_back((uint32_t)i);
            s_carry_ents.push_back(nwii::runtime::gx::FifoParser::kEntryUnknown);
        }
    if (prev_frame_end >= 0) {  // r40d: counters of the old path ([D40])
        ++dfast::st_old_parts;
        dfast::st_old_frames += s_prep_pending.dropped - old_drop0;
        dfast::st_old_cmds += (uint64_t)prev_frame_end + 1;
        dfast::st_old_ticks += wprof::Tick() - old_t0;
    }
    }

    // NWII_FRAMESIG: per-frame signature (draw count + TEV colour/konst regs
    // + alpha test). If the WARNING "press" blink exists in our command
    // stream, some field here must alternate between frames. (Reads g_state:
    // keeps the drain on the render thread.)
    static const bool framesig = NWII_ENV("NWII_FRAMESIG") != nullptr;
    if (framesig && s_prep_frames < 240) {
        size_t draws = 0, verts = 0;
        for (const auto &c : frame)
            if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive) {
                draws++;
                verts += c.raw ? c.raw->count : c.verts().size();
            }
        // Sampled hash of guest MEM1: distinguishes "game logic frozen" from
        // "logic running but the screen is legitimately static".
        uint32_t ramsig = 0;
        if (nwii::runtime::g_mmu) {
            for (uint32_t a = 0x80003000; a < 0x81800000; a += 4096)
                ramsig = ramsig * 33 + nwii::runtime::g_mmu->read32(a);
        }
        // Hash the actual draw payloads and XF matrix payloads: counts alone
        // can't distinguish "static screen" from "animation we drop".
        uint32_t drawsig = 0, xfsig = 0;
        for (const auto &c : frame) {
            if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive && c.raw)
                for (size_t i = 0; i < c.raw->bytes.size(); i += 4)
                    drawsig = drawsig * 33 + c.raw->bytes[i];
            if (c.type == nwii::runtime::gx::GXCommandType::XFRegister)
                for (float f : c.payload) {
                    uint32_t u; std::memcpy(&u, &f, 4);
                    xfsig = xfsig * 33 + u;
                }
        }
        uint32_t tevsig = 0;
        for (int r = 0xE0; r <= 0xE7; r++) tevsig = tevsig * 33 + g_state.bp[r];
        std::cout << "[FRAMESIG] f=" << s_prep_frames << " draws=" << draws
                  << " verts=" << verts << " tev=" << std::hex << tevsig
                  << " draw=" << drawsig << " xf=" << xfsig
                  << " ram=" << ramsig
                  << " alpha=" << g_state.bp[0xF3] << " cmode=" << g_state.bp[0x41]
                  << std::dec << "\n";
    }

    PH(3);
    st.mark(wprof::kSplit);
    s_hw_cmds.note(std::max(frame.size(), s_carry.size()), t1);
    ++s_prep_frames;
    // Decode only the surviving frame's draws (stale frames' draws were
    // dropped above and their vertices are never built).
    auto d0 = std::chrono::steady_clock::now();
    ++nwii::runtime::gx::g_gx_drain_epoch;  // vertex-array hashes are memoised per drain
    // The decode-cache check of each draw is bound by cache misses on its
    // DrawRaw and that DrawRaw's range list (12k draws per frame in Peach's
    // party, scattered on the heap): collect the draws, then prefetch a few
    // ahead in two stages (DrawRaw first, its range list once it is cached).
    // (s_draws, the frame's draws in order, was filled with the frame above.)
    static const bool s_r7check = NWII_ENV("NWII_R7CHECK") != nullptr;
    if (s_r7check) {
        size_t k = 0;
        bool ok = true;
        for (size_t fi = fstart; fi < frame.size(); ++fi) {  // r40d: from the kept frame
            auto &c = frame[fi];
            if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive)
                ok = ok && k < s_draws.size() && s_draws[k++] == &c;
        }
        ok = ok && k == s_draws.size();
        static uint64_t checked = 0, bad = 0, nty[4], npay;
        for (auto &c : frame) { ++nty[(int)c.type & 3]; npay += c.payload.size(); }
        if (checked % 600 == 599) {
            printf("[R7CHECK] per frame: bp=%llu cp=%llu xf=%llu draw=%llu xf_words=%llu\n",
                   (unsigned long long)nty[0] / 600, (unsigned long long)nty[1] / 600,
                   (unsigned long long)nty[2] / 600, (unsigned long long)nty[3] / 600,
                   (unsigned long long)npay / 600);
            for (auto &x : nty) x = 0;
            npay = 0;
        }
        ++checked;
        if (!ok) ++bad;
        if (!ok || checked % 600 == 0)
            printf("[R7CHECK] frames=%llu draw-list mismatches=%llu\n",
                   (unsigned long long)checked, (unsigned long long)bad);
    }
    const size_t nd = s_draws.size();
    const bool wprof_split = wprof::Level() >= 2;
    s_draw_entries.resize(nd, nwii::runtime::gx::FifoParser::kEntryUnknown);
    s_draw_pack.clear();
    // R22 R7: only the draws whose entry the parser did not make (draws outside
    // display lists, display-list misses; all of them with the index off).
    static std::vector<uint32_t> s_todo;
    s_todo.clear();
    for (size_t i = 0; i < nd; ++i)
        if (s_draw_entries[i] == nwii::runtime::gx::FifoParser::kEntryUnknown) s_todo.push_back((uint32_t)i);
    const size_t nt = s_todo.size();
    bool render_frees = true;
    // r40d NWII_D_RENDERPREP=1 (worker only): FIFO draws with all-direct attributes are
    // decoded and packed by the render thread (core 0, idle ~4-5 ms a frame in agua) right
    // before it draws the frame, with the same functions and inputs (their own bytes and
    // parse-time snapshot); their DrawRaw belongs to the command alone, so freeing it on
    // the render thread shares no count with the display-list cache.
    static const bool s_rprep_on = [] {
        const char *e = NWII_ENV("NWII_D_RENDERPREP");
        const bool v = e && e[0] && e[0] != '0';
        if (v) std::printf("[D40] decode+empaquetado de los draws inmediatos en el hilo de render: NWII_D_RENDERPREP=1\n");
        return v;
    }();
    static std::vector<uint32_t> s_rprep;
    s_rprep.clear();
    static const bool s_dmcheck = nwii::runtime::gx::FifoParser::MergeCheckOn();
    static std::vector<std::pair<uint32_t, std::vector<uint16_t>>> s_dmerges;
    s_dmerges.clear();
    const bool rprep = s_rprep_on && !apply_now;
    // r40d NWII_D_STATS=1 (diagnóstico): draws del fotograma por tipo ([D40S] cada ~2 s).
    static const bool s_dstats = NWII_ENV("NWII_D_STATS") != nullptr;
    if (s_dstats) {
        static uint64_t f = 0, a = 0, t = 0, imm = 0, dl = 0, verts = 0, bytes = 0, hit = 0, cmds = 0, same = 0, idx = 0;
        static std::unordered_set<uint64_t> *prev = new std::unordered_set<uint64_t>, *cur = new std::unordered_set<uint64_t>;
        static auto last = std::chrono::steady_clock::now();
        ++f; a += nd; t += nt; cmds += s_frame.size() - fstart;
        cur->clear();
        for (size_t q = 0; q < nt; ++q) {
            const auto &c = *s_draws[s_todo[q]];
            if (const auto *r = c.raw.get()) {
                (r->in_dl ? dl : imm)++;
                verts += r->count;
                bytes += r->bytes.size();
                if (r->decoded) ++hit;
                if (!r->in_dl) {
                    uint64_t h = 1469598103934665603ull;
                    auto mix = [&](const void *p, size_t n) {
                        const uint8_t *b = (const uint8_t *)p;
                        for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
                    };
                    mix(r->bytes.data(), r->bytes.size());
                    mix(&r->vat, sizeof r->vat);
                    mix(&r->count, sizeof r->count);
                    mix(&c.prim_type, 1);
                    mix(&r->defPosMtxIdx, 1);
                    mix(r->defTexMtxIdx, 8);
                    const auto &v = r->vat;
                    const bool ix = v.posMask == nwii::runtime::gx::VtxAttrMask::Index8 ||
                                    v.posMask == nwii::runtime::gx::VtxAttrMask::Index16 ||
                                    v.clrMask[0] == nwii::runtime::gx::VtxAttrMask::Index8 ||
                                    v.clrMask[0] == nwii::runtime::gx::VtxAttrMask::Index16 ||
                                    v.texMask[0] == nwii::runtime::gx::VtxAttrMask::Index8 ||
                                    v.texMask[0] == nwii::runtime::gx::VtxAttrMask::Index16;
                    if (ix) { ++idx; mix(r->arrayBase, sizeof r->arrayBase); mix(r->arrayStride, sizeof r->arrayStride); }
                    cur->insert(h);
                    if (prev->count(h)) ++same;
                    {
                        static std::map<std::string, uint64_t> sig;
                        char b[160];
                        int tm = 0, tx = 0;
                        for (int k = 0; k < 8; ++k) { tm |= v.texMatIdx[k] << k; tx |= (v.texMask[k] != nwii::runtime::gx::VtxAttrMask::None) << k; }
                        std::snprintf(b, sizeof b, "pmi%d tmi%02x pos%d/%d/%d/s%d nrm%d/%d clr%d/%d clr1_%d tex%02x t0:%d/%d/%d/s%d n=%u prim%02x",
                                      v.posMatIdx, tm, (int)v.posMask, (int)v.posType, v.posElements, v.posShift,
                                      (int)v.nrmMask, (int)v.nrmType, (int)v.clrMask[0], (int)v.clrType[0], (int)v.clrMask[1], tx,
                                      (int)v.texMask[0], (int)v.texType[0], v.texElements[0], v.texShift[0], r->count, c.prim_type);
                        ++sig[b];
                        static uint64_t ns = 0;
                        if (++ns % 200000 == 0) {
                            std::vector<std::pair<uint64_t, std::string>> o;
                            for (auto &kv : sig) o.push_back({kv.second, kv.first});
                            std::sort(o.rbegin(), o.rend());
                            for (size_t i = 0; i < o.size() && i < 12; ++i)
                                std::printf("[D40V] %8llu %s\n", (unsigned long long)o[i].first, o[i].second.c_str());
                            sig.clear();
                        }
                    }
                }
            }
        }
        std::swap(prev, cur);
        {
            // r40d diagnostic: runs of FIFO draws back to back in the frame (no command between),
            // same primitive and same snapshot (VAT, arrays, default matrices): mergeable candidates.
            static uint64_t rf = 0, rdraws = 0, radj = 0, rsame = 0, runs = 0, rmax = 0, rcur = 0, between = 0;
            const nwii::runtime::gx::GXCommand *prevd = nullptr;
            ++rf;
            for (size_t fi = fstart; fi < frame.size(); ++fi) {
                const auto &c = frame[fi];
                const bool imm = c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive && c.raw && !c.raw->in_dl;
                if (!imm) { if (prevd) ++between; prevd = nullptr; rcur = 0; continue; }
                ++rdraws;
                if (prevd) {
                    ++radj;
                    const auto &a = *prevd->raw, &b = *c.raw;
                    if (prevd->prim_type == c.prim_type && !std::memcmp(&a.vat, &b.vat, sizeof a.vat) &&
                        !std::memcmp(a.arrayBase, b.arrayBase, sizeof a.arrayBase) &&
                        !std::memcmp(a.arrayStride, b.arrayStride, sizeof a.arrayStride) &&
                        a.defPosMtxIdx == b.defPosMtxIdx && !std::memcmp(a.defTexMtxIdx, b.defTexMtxIdx, 8) &&
                        a.have_snap_mtx == b.have_snap_mtx) {
                        ++rsame;
                        rmax = std::max(rmax, ++rcur);
                    } else {
                        rcur = 0;
                    }
                } else {
                    ++runs;
                }
                prevd = &c;
            }
            if (rf >= 120) {
                std::printf("[D40M] por fotograma: draws FIFO %.0f, pegados al anterior %.0f (mismo formato/estado %.0f),"
                            " tramos %.0f, tramo max %llu\n", rdraws / (double)rf, radj / (double)rf, rsame / (double)rf,
                            runs / (double)rf, (unsigned long long)rmax + 1);
                rf = rdraws = radj = rsame = runs = rmax = rcur = between = 0;
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(2)) {
            last = now;
            std::printf("[D40S] por fotograma preparado: ordenes %.0f draws %.0f, sin entrada (todo) %.0f: inmediatos %.0f,"
                        " de lista %.0f (con decodificado previo %.0f), vertices %.0f, bytes %.0f | inmediatos iguales a uno"
                        " del fotograma anterior %.0f, con atributos indexados %.0f\n",
                        cmds / (double)f, a / (double)f, t / (double)f, imm / (double)f, dl / (double)f, hit / (double)f,
                        verts / (double)f, bytes / (double)f, same / (double)f, idx / (double)f);
            f = a = t = imm = dl = verts = bytes = hit = cmds = same = idx = 0;
        }
    }
    for (size_t t = 0; t < nt; ++t) {
        const size_t i = s_todo[t];
        if (t + 8 < nt) {
            if (auto *r = s_draws[s_todo[t + 8]]->raw.get()) {
                __builtin_prefetch(r);  // make_shared: refcounts sit just before it
                __builtin_prefetch(&r->count);
                __builtin_prefetch(&r->decoded);
                __builtin_prefetch(&r->ranges_hash);
            }
        }
        if (t + 4 < nt) {
            if (auto *r = s_draws[s_todo[t + 4]]->raw.get()) {
                if (!r->ranges.empty()) __builtin_prefetch(r->ranges.data());
                if (r->decoded) __builtin_prefetch(r->decoded.get());
            }
        }
        if (s_dmcheck) {  // r40d NWII_D_MERGE_CHECK: the parts of a merged draw, before its DrawRaw goes
            const auto *r = s_draws[i]->raw.get();
            if (r && !r->merge_counts.empty()) s_dmerges.push_back({(uint32_t)i, r->merge_counts});
        }
        if (rprep && nwii::runtime::gx::FifoParser::RenderPrepOk(*s_draws[i])) {  // r40d
            s_rprep.push_back((uint32_t)i);
            s_draw_entries[i] = nullptr;
            continue;
        }
        nwii::runtime::gx::FifoParser::DecodeDraw(*s_draws[i]);
        if (wprof_split) st.mark(wprof::kDecode);
        s_draw_entries[i] = nwii::runtime::gx::GX_PrepDrawForRender(*s_draws[i], s_draw_pack);  // R14 R5
        if (wprof_split) st.mark(wprof::kPrep);
        {
            // R22 R7: a draw still holding a DrawRaw or owning decoded vertices
            // may share their (non-atomic) counts with the display-list cache.
            const auto &c = *s_draws[i];
            if (c.raw || (c.shared_vertices && c.shared_vertices.use_count() > 0)) render_frees = false;
        }
    }
    // R22 R7: the next frame's display-list replays check vertex memory afresh
    // (RangesHash memoises chunk hashes per epoch).
    ++nwii::runtime::gx::g_gx_drain_epoch;
    s_prep_pending.draws += nd;
    if (!wprof_split) st.mark(wprof::kDecode);
    wprof::s_frames.fetch_add(1, std::memory_order_relaxed);
    wprof::s_cmds.fetch_add(s_frame.size() - fstart, std::memory_order_relaxed);
    wprof::s_draws.fetch_add(nd, std::memory_order_relaxed);
    PH(4);
    auto d1 = std::chrono::steady_clock::now();
    s_prep_pending.decode_us +=
        std::chrono::duration_cast<std::chrono::microseconds>(d1 - d0).count();

    // Hand the frame over: the packet's (empty) vectors become the next
    // frame's, so their capacity is reused.
    pkt.frame.swap(s_frame);
    pkt.draws.swap(s_draws);
    pkt.entries.swap(s_draw_entries);
    pkt.pack.swap(s_draw_pack);
    pkt.start = fstart;  // r40d
    pkt.rprep.swap(s_rprep);  // r40d NWII_D_RENDERPREP
    pkt.merges.swap(s_dmerges);  // r40d NWII_D_MERGE_CHECK
    s_dmerges.clear();
    {
        static uint64_t m0 = 0, f = 0;  // r40d NWII_D_MERGE: [D40G] FIFO draws merged per prepared frame
        static auto last = std::chrono::steady_clock::now();
        ++f;
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(2)) {
            last = now;
            const uint64_t m = nwii::runtime::gx::FifoParser::Merged();
            if (m != m0)
                std::printf("[D40G] draws del FIFO fundidos con el anterior: %.0f por fotograma preparado\n",
                            (m - m0) / (double)f);
            m0 = m;
            uint64_t g, pa, mk, bd;  // r45p NWII_P_DLMERGE
            nwii::runtime::gx::FifoParser::DLMergeStats(g, pa, mk, bd);
            if (g || mk)
                std::printf("[P45DL] por fotograma preparado: %.0f draws de listas fundidos en %.0f | grupos hechos %llu, "
                            "nunca fundibles %llu\n", pa / (double)f, g / (double)f, (unsigned long long)mk,
                            (unsigned long long)bd);
            f = 0;
        }
    }
    s_rprep.clear();
    pkt.seq = s_prep_frames;
    pkt.has_draws = !pkt.draws.empty();
    // (the draws not in the todo list are display-list memo draws: never owning)
    pkt.render_frees = render_frees && use_idx && nwii::runtime::gx::FifoParser::DLMemoOn();
    {
        // R22 R7: free the retired display-list vertices no live frame can
        // point at (this side alone writes seq / has_draws).
        static FramePacket *s_seen[4];
        for (int i = 0; i < 4; ++i) {
            if (s_seen[i] == &pkt) break;
            if (!s_seen[i]) { s_seen[i] = &pkt; break; }
        }
        uint64_t min_live = s_prep_frames + 1;
        for (FramePacket *p : s_seen)
            if (p && p->has_draws && p->seq < min_live) min_live = p->seq;
        nwii::runtime::gx::FifoParser::ReleaseRetired(min_live);
    }
    if (!apply_now) {
        nwii::runtime::gx::FifoParser::TakeRenderState(pkt.state);
        if (retracted) {
            // The retracted frame's BP seeds were never applied: they go
            // first, this parse's override them (as one drain of both).
            auto &n = pkt.state;
            const auto &o = *s_prev_state;
            for (int w = 0; w < 4; ++w) {
                for (uint64_t m = o.seed_mask[w] & ~n.seed_mask[w]; m; m &= m - 1) {
                    const int r = w * 64 + __builtin_ctzll(m);
                    n.seed_bp[r] = o.seed_bp[r];
                }
                n.seed_mask[w] |= o.seed_mask[w];
            }
            for (int k = 0; k < 8; ++k) {
                if ((o.seed_kmask & ~n.seed_kmask) & (1u << k))
                    n.seed_konst[k / 2][k & 1] = o.seed_konst[k / 2][k & 1];
                if ((o.seed_cmask & ~n.seed_cmask) & (1u << k))
                    n.seed_color[k / 2][k & 1] = o.seed_color[k / 2][k & 1];
            }
            n.seed_kmask |= o.seed_kmask;
            n.seed_cmask |= o.seed_cmask;
        }
    }
    {
        namespace fp = nwii::runtime::gx;
        PrepStats &p = s_prep_pending;
        p.snap_draws = fp::g_prof_draws; fp::g_prof_draws = 0;
        p.draw_bytes = fp::g_prof_draw_bytes; fp::g_prof_draw_bytes = 0;
        p.dl_calls = fp::g_prof_dl_calls; fp::g_prof_dl_calls = 0;
        p.dl_bytes = fp::g_prof_dl_bytes; fp::g_prof_dl_bytes = 0;
        p.unknown = fp::g_prof_unknown; fp::g_prof_unknown = 0;
        p.dl_us = fp::g_prof_dl_us; fp::g_prof_dl_us = 0;
        p.snap_us = fp::g_prof_snap_us; fp::g_prof_snap_us = 0;
        p.dl_loadindx = fp::g_prof_dl_loadindx; fp::g_prof_dl_loadindx = 0;
        p.dl_hits = fp::g_prof_dl_hits; fp::g_prof_dl_hits = 0;
        p.decode_hits = fp::g_prof_decode_hits; fp::g_prof_decode_hits = 0;
        p.carry = s_carry.size();
        p.buf = s_parse_buf.size();
    }
    s_prep_pending.prep_cpu_us += thread_cpu_us() - cpu0;
    prep_cpu.armed = false;
    st.mark(wprof::kHand);
    prev_stats.add(s_prep_pending);  // (prev_stats is empty unless retracted)
    pkt.stats = prev_stats;
    s_prep_pending = PrepStats{};
    if (!apply_now) {
        // Worker: memory figures for GX_MemReport every ~2 s.
        static auto last = std::chrono::steady_clock::time_point{};
        if (d1 - last >= std::chrono::seconds(2)) {
            last = d1;
            const PrepMem m = ComputePrepMem();
            std::lock_guard<std::mutex> lk(s_prep_mem_m);
            s_prep_mem = m;
        }
    }
    rcoal::Report();  // r36r
    dfast::Report();  // r40d
    return &pkt;
}

// [RPROF] / [GXPROF] once every 2 s (render thread).
static void PrintProf() {
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count() < 2) return;
    last = now;
    extern uint64_t g_prof_gl_draws, g_prof_gx_draws, g_prof_stitches;
    extern uint64_t g_prof_shader_compiles, g_prof_shader_loads,
        g_prof_shader_us, g_prof_shader_skips;
    extern uint64_t nwii_stat_shaders();
    extern uint64_t g_prof_tex_uploads, g_prof_tex_bind_saves;
    extern uint64_t g_prof_decode_us_accum;
    const uint64_t rf = g_prof_render_frames ? g_prof_render_frames : 1;
    std::cout << "[RPROF] frames=" << g_prof_render_frames
              << " render_us/frame=" << g_prof_render_us / rf
              << " render_max_us=" << g_prof_render_max_us
              << " render_cpu_us/frame=" << g_prof_render_cpu_us / rf
              << " drain_cpu_us/frame=" << g_prof_drain_cpu_us / rf
              << " gx_draws/frame=" << g_prof_gx_draws / rf
              << " gl_draws/frame=" << g_prof_gl_draws / rf
              << " stitches/frame=" << g_prof_stitches / rf
              << " shaders: new=" << g_prof_shader_compiles
              << " disk=" << g_prof_shader_loads
              << " ms=" << g_prof_shader_us / 1000
              << " skipped_draws=" << g_prof_shader_skips
              << " programs=" << nwii_stat_shaders()
              << " dropped_frames=" << g_prof_dropped_frames
              << " tex_uploads=" << g_prof_tex_uploads
              << " tex_bind_fixes=" << g_prof_tex_bind_saves;
    if (g_prof_gpu_frames)
        std::cout << " gpu_us/frame=" << g_prof_gpu_us / g_prof_gpu_frames;
    // R11: preparing half (parse, decode) per frame, on the render worker
    // (worker=1) or inside drain_cpu (worker=0).
    std::cout << " prep_cpu_us/frame=" << s_prof_acc.prep_cpu_us / rf
              << " worker=" << (s_worker_on.load(std::memory_order_relaxed) ? 1 : 0)
              << " retracts=" << s_prof_acc.retracts << " held=" << [] {
                     std::lock_guard<FifoLock> lock(g_fifo_mutex);
                     return s_held_frames;
                 }();
    std::cout << "\n";
    {
        // R14 R5: [PIPE] (see PipeNoteXfb)
        auto ms = [](uint64_t ns, uint64_t n) { return n ? ns / 1e6 / n : 0.0; };
        const uint64_t xn = s_pipe_xfb_n.exchange(0), xs = s_pipe_xfb_sum.exchange(0),
                       xm = s_pipe_xfb_max.exchange(0);
        uint32_t h[5];
        for (int i = 0; i < 5; ++i) h[i] = s_pipe_xfb_h[i].exchange(0);
        const uint64_t wn = s_pipe_w_n.exchange(0), ws = s_pipe_w_sum.exchange(0),
                       wm = s_pipe_w_max.exchange(0);
        char b[640];
        std::snprintf(b, sizeof b,
                      "[PIPE] game frames=%llu interval avg=%.1f max=%.1f ms (<=18:%u <=22:%u <=30:%u <=36:%u >36:%u)"
                      " | worker frame-end->ready avg=%.1f max=%.1f ms (n=%llu)"
                      " | render ready->taken avg=%.1f max=%.1f, draw avg=%.1f max=%.1f ms (n=%llu)"
                      " | swap avg=%.1f max=%.1f ms, loop avg=%.1f max=%.1f ms (n=%llu)\n",
                      (unsigned long long)xn, ms(xs, xn), xm / 1e6, h[0], h[1], h[2], h[3], h[4],
                      ms(ws, wn), wm / 1e6, (unsigned long long)wn, ms(s_pipe_r_wait, s_pipe_r_n),
                      s_pipe_r_wait_max / 1e6, ms(s_pipe_r_draw, s_pipe_r_n), s_pipe_r_draw_max / 1e6,
                      (unsigned long long)s_pipe_r_n, ms(s_pipe_s_sum, s_pipe_s_n), s_pipe_s_max / 1e6,
                      ms(s_pipe_l_sum, s_pipe_l_n), s_pipe_l_max / 1e6, (unsigned long long)s_pipe_l_n);
        std::cout << b;
        vtl::Dump();
        {
            // r37v NWII_V_LATE: retrazos retenidos en estos ~2 s (contadores del hilo del juego, ios.cpp)
            static uint64_t n0 = 0, i0 = 0, t0 = 0, d0 = 0, u0 = 0;
            const uint64_t n = nwii::runtime::g_v_held_n, i = nwii::runtime::g_v_held_idle_n,
                           t = nwii::runtime::g_v_held_to_n, d = nwii::runtime::g_v_held_drop_n,
                           u = nwii::runtime::g_v_held_us;
            if (n != n0)
                std::printf("[V] retrazos retenidos %llu (lanzados al esperar %llu, por plazo %llu, perdidos %llu),"
                            " retencion media %.2f ms\n",
                            (unsigned long long)(n - n0), (unsigned long long)(i - i0), (unsigned long long)(t - t0),
                            (unsigned long long)(d - d0), (i + t > i0 + t0) ? (u - u0) / 1000.0 / (double)(i + t - i0 - t0) : 0.0);
            n0 = n; i0 = i; t0 = t; d0 = d; u0 = u;
        }
        s_pipe_r_n = s_pipe_r_wait = s_pipe_r_wait_max = s_pipe_r_draw = s_pipe_r_draw_max = 0;
        s_pipe_s_n = s_pipe_s_sum = s_pipe_s_max = s_pipe_l_n = s_pipe_l_sum = s_pipe_l_max = 0;
    }
    g_prof_gpu_us = g_prof_gpu_frames = 0;
    g_prof_dropped_frames = 0;
    g_prof_tex_bind_saves = 0;
    g_prof_tex_uploads = 0;
    g_prof_shader_compiles = g_prof_shader_loads = 0;
    g_prof_shader_us = g_prof_shader_skips = 0;
    g_prof_render_us = g_prof_render_frames = 0;
    g_prof_render_max_us = 0;
    g_prof_render_cpu_us = g_prof_drain_cpu_us = 0;
    g_prof_stitches = 0;
    g_prof_gl_draws = g_prof_gx_draws = 0;
    const PrepStats &a = s_prof_acc;
    std::cout << "[GXPROF] parse+decode_ms=" << (a.parse_us + a.decode_us) / 1000
              << " (decode_ms=" << g_prof_decode_us_accum / 1000 << ")"
              << " draws_snap=" << a.snap_draws
              << " draw_bytes=" << a.draw_bytes
              << " dl_calls=" << a.dl_calls
              << " dl_bytes=" << a.dl_bytes
              << " dl_ms=" << a.dl_us / 1000
              << " snap_ms=" << a.snap_us / 1000
              << " dl_loadindx=" << a.dl_loadindx
              << " dl_hits=" << a.dl_hits
              << " decode_hits=" << a.decode_hits
              << " unknown=" << a.unknown
              << " carry=" << a.carry
              << " buf=" << a.buf << "\n";
    g_prof_decode_us_accum = 0;
    s_prof_acc = PrepStats{};
    {
        // R22 R7: [WPROF] the preparing side per prepared frame (us), see wprof::.
        const uint64_t f = wprof::s_frames.exchange(0), calls = wprof::s_calls.exchange(0),
                       cmds = wprof::s_cmds.exchange(0), dr = wprof::s_draws.exchange(0),
                       busy = wprof::s_busy.exchange(0);
        const double fd = f ? (double)f : 1.0;
        char b[768];
        int n = std::snprintf(b, sizeof b, "[WPROF] frames=%llu calls=%llu busy_us/frame=%.0f",
                              (unsigned long long)f, (unsigned long long)calls, wprof::TickUs(busy) / fd);
        for (int i = 0; i < wprof::kN; ++i)
            n += std::snprintf(b + n, sizeof b - n, " %s=%.0f", wprof::kName[i],
                               wprof::TickUs(wprof::s_acc[i].exchange(0)) / fd);
        n += std::snprintf(b + n, sizeof b - n,
                           " | cmds/frame=%.0f draws/frame=%.0f dl_hit_us/frame=%.0f dl_hit_cmds/frame=%.0f dl_miss_us/frame=%.0f",
                           cmds / fd, dr / fd, wprof::s_dlhit_us.exchange(0) / 1000.0 / fd,
                           wprof::s_dlhit_cmds.exchange(0) / fd, wprof::s_dlmiss_us.exchange(0) / 1000.0 / fd);
        {
            const uint64_t rf = wprof::s_rfree.exchange(0), rns = wprof::s_rfree_ns.exchange(0);
            n += std::snprintf(b + n, sizeof b - n, " | render_freed=%llu (%.0f us each)\n",
                               (unsigned long long)rf, rf ? rns / 1000.0 / rf : 0.0);
        }
        std::cout << b;
    }
}

// The rendering half: one prepared frame (render thread).
static void RenderPacket(FramePacket &pkt, bool worker) {
    extern uint64_t g_prof_decode_us_accum;
    ++g_stat_frames;
    g_stat_draws += pkt.stats.draws;
    g_stat_parse_us += pkt.stats.parse_us + pkt.stats.decode_us;
    g_prof_dropped_frames += pkt.stats.dropped;
    g_prof_decode_us_accum += pkt.stats.decode_us;
    s_prof_acc.add(pkt.stats);
    PrintProf();
    if (worker) nwii::runtime::gx::FifoParser::ApplyRenderState(pkt.state);

    if (g_window) {
        if (!g_renderer) {
            g_renderer = nwii::runtime::gx::IRenderer::Create();
            g_renderer->Initialize(nullptr);
        }
        if (g_renderer) {
            auto r0 = std::chrono::steady_clock::now();
            const uint64_t rc0 = thread_cpu_us();
            const bool gpuprof = g_gpu_timer.on();
            if (gpuprof) g_gpu_timer.begin();
            if (!pkt.rprep.empty()) {
                // r40d NWII_D_RENDERPREP: what the worker left for this thread (see PrepFrame).
                static uint64_t n = 0, ns = 0, fr = 0;
                static auto last = std::chrono::steady_clock::now();
                const auto p0 = std::chrono::steady_clock::now();
                for (const uint32_t i : pkt.rprep) {
                    auto &c = *pkt.draws[i];
                    nwii::runtime::gx::FifoParser::DecodeDraw(c);
                    pkt.entries[i] = nwii::runtime::gx::GX_PrepDrawForRender(c, pkt.pack);
                }
                const auto p1 = std::chrono::steady_clock::now();
                n += pkt.rprep.size();
                ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(p1 - p0).count();
                ++fr;
                pkt.rprep.clear();
                if (p1 - last >= std::chrono::seconds(2)) {
                    last = p1;
                    std::printf("[D40R] preparados en el render: %.0f draws/fot, %.0f us/fot\n", n / (double)fr,
                                ns / 1000.0 / (double)fr);
                    n = ns = fr = 0;
                }
            }
            nwii::runtime::gx::g_render_draws = &pkt.draws;
            nwii::runtime::gx::g_render_entries =
                pkt.entries.size() == pkt.draws.size() ? &pkt.entries : nullptr;
            nwii::runtime::gx::g_render_pack = &pkt.pack;
            nwii::runtime::gx::g_render_prefix = &pkt.pre;  // r36r (empty unless NWII_R_PREFIX)
            nwii::runtime::gx::g_render_start = pkt.start;  // r40d (0 unless NWII_D_DROPFAST)
            // r40d NWII_D_MERGE_CHECK=<n>: every n-th frame with merged draws, the renderer draws it
            // also from a copy where each merged draw is split back into its parts, and compares.
            static std::vector<nwii::runtime::gx::GXCommand> s_alt;
            static long s_mc_every = [] {
                const char *e = NWII_ENV("NWII_D_MERGE_CHECK");
                return e ? std::max(1L, std::atol(e)) : 0L;
            }();
            static long s_mc_n = 0;
            // r45p NWII_P_DLMERGE_CHECK=<n>: also every n-th frame, display-list draws fused by
            // NWII_P_DLMERGE (DecodedVerts::parts) split back into their parts (same [D40MERGE_CHECK] line).
            static const long s_pdl_every = [] {
                const char *e = NWII_ENV("NWII_P_DLMERGE_CHECK");
                return e ? std::max(1L, std::atol(e)) : 0L;
            }();
            static long s_pdl_n = 0;
            const bool pdl_now = s_pdl_every && (++s_pdl_n % s_pdl_every) == 0;
            if ((s_mc_every && !pkt.merges.empty() && (++s_mc_n % s_mc_every) == 0) || pdl_now) {
                using nwii::runtime::gx::GXCommand;
                using nwii::runtime::gx::GXCommandType;
                std::unordered_map<const GXCommand *, const std::vector<uint16_t> *> parts;
                for (const auto &m : pkt.merges) parts[pkt.draws[m.first]] = &m.second;
                s_alt.clear();
                bool ok = true;
                for (size_t fi = pkt.start; fi < pkt.frame.size() && ok; ++fi) {
                    const GXCommand &c = pkt.frame[fi];
                    if (c.type != GXCommandType::DrawPrimitive) {
                        GXCommand &n = s_alt.emplace_back();
                        n.type = c.type; n.prim_type = c.prim_type; n.stale_copy = c.stale_copy; n.reg = c.reg;
                        n.length = c.length; n.val = c.val; n.bp_raw = c.bp_raw; n.payload = c.payload;
                        continue;
                    }
                    if (c.raw) { ok = false; break; }  // not decoded: no comparison for this frame
                    auto it = parts.find(&c);
                    const std::vector<nwii::runtime::gx::VertexData> &v = c.verts();
                    if (it == parts.end() && c.shared_vertices && !c.shared_vertices->parts.empty()) {  // r45p
                        size_t at = 0;
                        for (const uint16_t k : c.shared_vertices->parts) {
                            if (at + k > v.size()) { ok = false; break; }
                            GXCommand &n = s_alt.emplace_back();
                            n.type = c.type; n.prim_type = c.prim_type; n.reg = c.reg; n.length = c.length; n.val = c.val;
                            n.vertices.assign(v.begin() + (ptrdiff_t)at, v.begin() + (ptrdiff_t)(at + k));
                            at += k;
                        }
                        if (at != v.size()) ok = false;
                        continue;
                    }
                    if (it == parts.end()) {
                        GXCommand &n = s_alt.emplace_back();
                        n.type = c.type; n.prim_type = c.prim_type; n.reg = c.reg; n.length = c.length; n.val = c.val;
                        if (c.shared_vertices)  // aliasing, no count touched (render thread)
                            n.shared_vertices = nwii::runtime::gx::HostPtr<const nwii::runtime::gx::DecodedVerts>(
                                nwii::runtime::gx::HostPtr<const nwii::runtime::gx::DecodedVerts>(), c.shared_vertices.get());
                        else
                            n.vertices = c.vertices;
                        continue;
                    }
                    size_t at = 0;
                    for (const uint16_t k : *it->second) {
                        if (at + k > v.size()) { ok = false; break; }
                        GXCommand &n = s_alt.emplace_back();
                        n.type = c.type; n.prim_type = c.prim_type; n.reg = c.reg; n.length = c.length; n.val = c.val;
                        n.vertices.assign(v.begin() + (ptrdiff_t)at, v.begin() + (ptrdiff_t)(at + k));
                        at += k;
                    }
                    if (at != v.size()) ok = false;
                }
                if (ok) nwii::runtime::gx::g_d_merge_alt = &s_alt;
            }
            // r31t (frente T), SOLO MEDIR: NWII_T_NORENDER=N dibuja 1 de cada N fotogramas
            // (el hilo de render queda casi parado) para ver cuanto frena al hilo del juego
            // el trabajo del render en los otros nucleos (cache L2 compartida, memoria).
            static const long s_t_norender = [] {
                const char *e = NWII_ENV("NWII_T_NORENDER");
                return e ? std::atol(e) : 0L;
            }();
            static long s_t_n = 0;
            if (s_t_norender <= 1 || (s_t_n++ % s_t_norender) == 0)
            g_renderer->Render(pkt.frame);
            nwii::runtime::gx::g_render_draws = nullptr;
            nwii::runtime::gx::g_render_entries = nullptr;
            nwii::runtime::gx::g_render_pack = nullptr;
            nwii::runtime::gx::g_render_prefix = nullptr;
            nwii::runtime::gx::g_render_start = 0;
            nwii::runtime::gx::g_d_merge_alt = nullptr;
            if (gpuprof) g_gpu_timer.end();
            g_prof_render_cpu_us += thread_cpu_us() - rc0;
            auto r1 = std::chrono::steady_clock::now();
            const uint64_t rus =
                std::chrono::duration_cast<std::chrono::microseconds>(r1 - r0)
                    .count();
            g_stat_render_us += rus;
            g_prof_render_us += rus;
            if (rus > g_prof_render_max_us) g_prof_render_max_us = rus;
            ++g_prof_render_frames;
        }
    }
    // Test knobs (R11): make the render thread slow, as on the Switch, so the
    // worker's frame replacement and the guest's backpressure get exercised on
    // a fast PC. NWII_R11_SLOW_RENDER_US=<us> after every frame;
    // NWII_R11_STALL=<frame>,<ms>: one long stall at that frame (a shader
    // compile on the Switch).
    static const long slow_us = [] {
        const char *e = NWII_ENV("NWII_R11_SLOW_RENDER_US");
        return e ? std::atol(e) : 0L;
    }();
    static long stall_frame = -1, stall_ms = 0;
    static const bool stall_init = [] {
        if (const char *e = NWII_ENV("NWII_R11_STALL")) std::sscanf(e, "%ld,%ld", &stall_frame, &stall_ms);
        return true;
    }();
    (void)stall_init;
    if (slow_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(slow_us));
    if (stall_frame >= 0 && (long)g_stat_frames == stall_frame) {
        std::printf("[GX] R11 test stall: %ld ms at frame %ld\n", stall_ms, stall_frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
    }
}

static void WorkerMain() {
    t_on_worker = true;
#ifndef __SWITCH__
    nwii::runtime::s2::pc_sched(4);  // r38c: NWII_C_PCSCHED (solo PC)
#endif
#ifdef __SWITCH__
    // Homebrew gets cores 0-2: render (main) on 0, the recompiled CPU on 1,
    // audio (AX mixer, audren output) on 2, a few % busy. One priority step
    // below the default (0x2C) so the AX mixer and the audio output always
    // preempt it. NWII_RENDER_WORKER_CORE / _PRIO override.
    {
        u64 allowed = 0;
        svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
        const char *ce = std::getenv("NWII_RENDER_WORKER_CORE");
        const int want = ce ? std::atoi(ce) : 2;
        int core = -1;
        for (int c : {want, 2, 0})
            if (c >= 0 && c < 3 && (allowed & (1ull << c)) &&
                R_SUCCEEDED(svcSetThreadCoreMask(CUR_THREAD_HANDLE, c, 1u << c))) {
                core = c;
                break;
            }
        const char *pe = std::getenv("NWII_RENDER_WORKER_PRIO");
        const int prio = pe ? (int)std::strtol(pe, nullptr, 0) : 0x2D;
        const Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, prio);
        std::printf("[NX] render worker thread on core %d, priority 0x%X%s (allowed mask 0x%llx)\n",
                    core, prio, R_SUCCEEDED(rc) ? "" : " (failed)", (unsigned long long)allowed);
        nwii::runtime::nx::note_thread(nwii::runtime::nx::kThreadGxw);
    }
#endif
    using clk = std::chrono::steady_clock;
    static const auto poll = std::chrono::microseconds([] {
        const char *e = NWII_ENV("NWII_PRESENT_POLL_US");
        long v = e ? std::atol(e) : 1000;
        return v < 100 ? 100L : v > 8000 ? 8000L : v;
    }());
    // The packet to fill once a frame end is parsed: the waiting one if the
    // render thread has not taken it (it is then replaced by a newer frame,
    // as the single-threaded drain always rendered the newest one), else the
    // one the render thread is not drawing.
    // NWII_NOFRAMESKIP renders every frame: wait for the render thread to take
    // the waiting one instead.
    static FramePacket *(*const acquire)(bool &) = [](bool &retracted) -> FramePacket * {
        static const bool noskip = NWII_ENV("NWII_NOFRAMESKIP") != nullptr;
        std::unique_lock<std::mutex> lk(s_pm);
        if (noskip) s_cv_worker.wait(lk, [] { return !s_ready || s_stop; });
        if (PTripleOn()) {  // r45p: one neither drawn nor waiting (never takes the waiting one back here)
            for (FramePacket &q : s_pkts)
                if (&q != s_rendering && &q != s_ready) return &q;
        }
        if (FramePacket *p = s_ready) {
            s_ready = nullptr;
            retracted = true;
            return p;
        }
        return s_rendering == &s_pkts[0] ? &s_pkts[1] : &s_pkts[0];
    };
    auto last_frame = clk::now();
#if !defined(__SWITCH__)
    // r40d, SOLO PRUEBAS EN PC: NWII_D_SLOWWORKER=<factor>[,<us>]: tras cada llamada a PrepFrame que trabajo, el
    // trabajador duerme (factor - 1) x lo que tardo (+ us si preparo un fotograma): un trabajador <factor> veces
    // mas lento, como el de la Switch (nucleo 2 compartido con la CPU de sonido); asi llegan lotes con dos
    // finales de fotograma y el coste del descarte realimenta el siguiente lote, como en la consola.
    static const double s_d_slow_f = [] {
        const char *e = std::getenv("NWII_D_SLOWWORKER");
        return e ? std::atof(e) : 0.0;
    }();
    static const long s_d_slow_us = [] {
        const char *e = std::getenv("NWII_D_SLOWWORKER");
        const char *c = e ? std::strchr(e, ',') : nullptr;
        return c ? std::atol(c + 1) : 0L;
    }();
#endif
    for (;;) {
#if !defined(__SWITCH__)
        const auto d_t0 = clk::now();
        const uint64_t d_f0 = s_prep_frames;
        const bool d_had = s_d_slow_f > 1.0 &&
                           (!s_parse_buf.empty() ||
                            [] { std::lock_guard<FifoLock> lock(g_fifo_mutex); return !g_hw_fifo.empty(); }());
#endif
        FramePacket *fill = PrepFrame(acquire, false);
#if !defined(__SWITCH__)
        if (d_had) {
            const auto el = clk::now() - d_t0;
            std::this_thread::sleep_for(
                std::chrono::duration_cast<std::chrono::microseconds>(el * (s_d_slow_f - 1.0)) +
                std::chrono::microseconds(s_prep_frames != d_f0 ? s_d_slow_us : 0L));
        }
#endif
        if (fill) {
            // r45p NWII_P_TRIPLE: the packet still waiting (if the render thread did not take it
            // meanwhile) is retracted into the new one now that the new one is complete.
            if (PTripleOn()) {
                FramePacket *cand = nullptr, *old = nullptr;
                {
                    std::lock_guard<std::mutex> lk(s_pm);
                    cand = s_ready;
                    s_p_reading = cand;
                }
                if (cand && cand != fill && PKeepCopyOn() && HasLoneCopy(*cand, *fill)) {
                    // r45p NWII_P_KEEPCOPY: let the render thread take it first (see PKeepCopyOn)
                    const int64_t k0 = PipeNow();
                    std::unique_lock<std::mutex> lk(s_pm);
                    s_p_reading = nullptr;
                    s_cv_worker.wait_for(lk, std::chrono::milliseconds(100), [&] { return s_ready != cand || s_stop; });
                    cand = nullptr;  // taken (or, after 100 ms, retracted as before: below it is still s_ready)
                    if (s_ready) { cand = s_ready; s_p_reading = cand; }
                    lk.unlock();
                    ++s_p_kept_frames;
                    s_p_keep_wait_ns += (uint64_t)(PipeNow() - k0);
                    std::printf("[P45K] fotograma con copia EFB unica conservado (espera %.1f ms; %llu en total)\n",
                                (PipeNow() - k0) / 1e6, (unsigned long long)s_p_kept_frames);
                }
                if (cand && cand != fill) CopyKept(*cand);  // the packet stays available meanwhile
                const int64_t w0 = PipeNow();
                {
                    std::lock_guard<std::mutex> lk(s_pm);
                    if (cand && cand != fill && s_ready == cand) {
                        old = cand;
                        s_ready = nullptr;
                    }
                    s_p_reading = nullptr;
                }
                if (!old) s_p_kept.clear();  // taken by the render thread meanwhile
                if (old) {
                    SpliceRetracted(*old, *fill);
                    const uint64_t d = (uint64_t)(PipeNow() - w0);
                    ++s_p_merges;
                    s_p_win_ns += d;
                    s_p_win_max = std::max(s_p_win_max, d);
                }
                if (++s_p_pub % 600 == 0) {
                    std::printf("[P45T] paquetes publicados %llu: fusiones %llu, ventana sin fotograma media %.0f us max %.0f us\n",
                                (unsigned long long)s_p_pub, (unsigned long long)s_p_merges,
                                s_p_merges ? s_p_win_ns / 1e3 / (double)s_p_merges : 0.0, s_p_win_max / 1e3);
                    s_p_merges = s_p_win_ns = s_p_win_max = 0;
                }
            }
            {
                const int64_t t = PipeNow();
                const int64_t x = s_pipe_xfb_last.load(std::memory_order_relaxed);
                if (x && t > x) {
                    const uint64_t d = (uint64_t)(t - x);
                    s_pipe_w_n.fetch_add(1, std::memory_order_relaxed);
                    s_pipe_w_sum.fetch_add(d, std::memory_order_relaxed);
                    if (d > s_pipe_w_max.load(std::memory_order_relaxed))
                        s_pipe_w_max.store(d, std::memory_order_relaxed);
                }
                fill->t_ready = t;
                std::lock_guard<std::mutex> lk(s_pm);
                if (__builtin_expect(vtl::g_on, 0))
                    vtl::NoteAt(t, 6, (uint32_t)fill->seq | (s_ready ? 0x80000000u : 0u));
                s_ready = fill;
            }
            s_cv_main.notify_one();
            last_frame = clk::now();
            if (!s_p_junk.empty()) s_p_junk.clear();  // r45p: the retracted commands, after the hand-over
        }
        // Look again in a poll interval (4 of them after 25 ms without a
        // frame), like the frontend's drain loop did.
        const auto d = clk::now() - last_frame > std::chrono::milliseconds(25) ? 4 * poll : poll;
        std::unique_lock<std::mutex> lk(s_pm);
        s_cv_worker.wait_for(lk, d, [] { return s_stop; });
        if (s_stop) return;
    }
}

static bool WorkerWanted() {
    if (!g_window) return false;  // headless: no render, nothing to overlap
    const char *e = NWII_ENV("NWII_RENDER_WORKER");
    if (e && e[0] == '0') {
        std::printf("[GX] render worker: off (NWII_RENDER_WORKER=0)\n");
        return false;
    }
    // Debug modes that read render-thread state while parsing, time the
    // drain phases on one thread, or signal the PE from the parser.
    static const char *const kInline[] = {"NWII_R7PH", "NWII_FRAMESIG", "NWII_LATE_SYNC"};
    for (const char *d : kInline)
        if (std::getenv(d)) {
            std::printf("[GX] render worker: off (%s)\n", d);
            return false;
        }
    return true;
}

void GX_StopWorker();
static void StartWorker() {
#if defined(__SWITCH__)
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 1u << 20);
    const int rc = pthread_create(
        &s_worker_thread, &attr, [](void *) -> void * { WorkerMain(); return nullptr; }, nullptr);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        std::printf("[GX] render worker: thread start failed (%d), drain on the render thread\n", rc);
        return;
    }
#else
    s_worker_thread = new std::thread(WorkerMain);
#endif
    s_worker_on.store(true);
    // An exit() elsewhere (the main loop calls GX_StopWorker itself) must not
    // run static destructors (display-list cache...) under a running worker.
    std::atexit([] { GX_StopWorker(); });
    std::printf("[GX] render worker: on (FIFO parse + decode on their own thread, whole frames "
                "handed to the render thread; NWII_RENDER_WORKER=0 disables)\n");
}

// Main loop, at exit (before the GL context goes): stop and join the worker.
void GX_StopWorker() {
    if (!s_worker_on.load()) return;
    {
        std::lock_guard<std::mutex> lk(s_pm);
        s_stop = true;
    }
    s_cv_worker.notify_all();
    s_cv_main.notify_all();
#if defined(__SWITCH__)
    pthread_join(s_worker_thread, nullptr);
#else
    s_worker_thread->join();
#endif
    s_worker_on.store(false);
}

// R14 R5: main loop timing for [PIPE]: the buffer swap and the loop period.
void GX_PipeNoteSwap(int64_t swap_ns) {
    static int64_t last = 0;
    const int64_t t = PipeNow();
    ++s_pipe_s_n;
    s_pipe_s_sum += (uint64_t)swap_ns;
    s_pipe_s_max = std::max(s_pipe_s_max, (uint64_t)swap_ns);
    if (last) {
        const uint64_t d = (uint64_t)(t - last);
        ++s_pipe_l_n;
        s_pipe_l_sum += d;
        s_pipe_l_max = std::max(s_pipe_l_max, d);
    }
    last = t;
}

// Frame-paced main loop: wait for the next drain. Without the worker, sleep
// `poll_us` (the drain polls the FIFO); with it, wake as soon as a frame is
// ready, at most `max_us` later.
void GX_HostWait(int64_t poll_us, int64_t max_us) {
    if (!s_worker_on.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::microseconds(poll_us));
        return;
    }
    std::unique_lock<std::mutex> lk(s_pm);
    s_cv_main.wait_for(lk, std::chrono::microseconds(max_us),
                       [] { return s_ready != nullptr || s_stop; });
}

// Seconds of CPU time the worker used so far (-1: no worker / unknown).
double GX_WorkerCpuSeconds() {
#if defined(__linux__) && !defined(__SWITCH__)
    if (!s_worker_on.load(std::memory_order_relaxed)) return -1;
    clockid_t cid;
    if (pthread_getcpuclockid(s_worker_thread->native_handle(), &cid) != 0) return -1;
    timespec ts{};
    clock_gettime(cid, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#else
    return -1;
#endif
}

namespace nwii::runtime::hle { bool r2_prehash_idle(bool (*more)()); bool r2_prehash_pending(); }  // r39r2 (texture_cache_gl.cpp)
static bool R2FrameWaiting() {
    std::lock_guard<std::mutex> lk(s_pm);
    return s_ready != nullptr;
}
void ProcessGXFifo() {
    static const bool worker = [] {
        if (WorkerWanted()) StartWorker();
        return s_worker_on.load();
    }();
    if (worker) {
        // r39r2 NWII_R2_LATEFREE=1: the commands of the frame drawn last are
        // freed here, on the next call (after the buffer swap), and only while
        // no new frame waits; else the packet goes back to the worker, which
        // frees them when it reuses it (as without NWII_RENDER_FREE). Before,
        // the render thread freed them between Render() and the swap (0,5-1 ms
        // in desierto/agua/cielo). The packet stays s_rendering meanwhile, so
        // the worker never fills it.
        if (s_late_pkt) {
            FramePacket *q = s_late_pkt;
            s_late_pkt = nullptr;
            bool waiting;
            {
                std::lock_guard<std::mutex> lk(s_pm);
                waiting = s_ready != nullptr || q == s_p_reading;  // r45p: the worker reads it: it frees it
            }
            if (!waiting) {
                const int64_t f0 = PipeNow();
                auto &fr = q->frame;
                bool stopped = false;
                while (!fr.empty()) {
                    const size_t k = std::min<size_t>(fr.size(), 2048);
                    fr.erase(fr.end() - (ptrdiff_t)k, fr.end());
                    if (fr.empty()) break;
                    std::lock_guard<std::mutex> lk(s_pm);
                    if (s_ready) { stopped = true; break; }
                }
                if (!stopped) {
                    q->draws.clear();
                    q->entries.clear();
                    q->pack.clear();
                    q->pre.clear();
                }
                wprof::s_rfree.fetch_add(1, std::memory_order_relaxed);
                wprof::s_rfree_ns.fetch_add((uint64_t)(PipeNow() - f0), std::memory_order_relaxed);
            }
            std::lock_guard<std::mutex> lk(s_pm);
            if (s_rendering == q) s_rendering = nullptr;  // the worker frees what is left
        }
        // r39r2 NWII_R2_PREHASH: idle (no frame waiting): hash the RAM ranges the
        // next frame will need (texture_cache_gl.cpp).
        if (nwii::runtime::hle::r2_prehash_pending() && !R2FrameWaiting())
            nwii::runtime::hle::r2_prehash_idle(R2FrameWaiting);
        FramePacket *p;
        {
            std::lock_guard<std::mutex> lk(s_pm);
            p = s_ready;
            if (!p) return;
            s_ready = nullptr;
            s_rendering = p;
        }
        {
            // Its frames are taken now (see s_held_frames).
            std::lock_guard<FifoLock> lock(g_fifo_mutex);
            s_held_frames -= (uint32_t)p->stats.held;
        }
        static const bool noskip = NWII_ENV("NWII_NOFRAMESKIP") != nullptr;
        if (noskip || PKeepCopyOn()) s_cv_worker.notify_one();  // the worker may wait for this (r45p: KEEPCOPY)
        const uint64_t c0 = thread_cpu_us();
        const int64_t t0 = PipeNow();
        if (__builtin_expect(vtl::g_on, 0)) vtl::NoteAt(t0, 7, (uint32_t)p->seq);
        RenderPacket(*p, true);
        if (__builtin_expect(vtl::g_on, 0)) vtl::NoteAt(PipeNow(), 8, (uint32_t)p->seq);
        g_prof_drain_cpu_us += thread_cpu_us() - c0;
        {
            // R22 R7: free the frame's commands here instead of on the worker
            // when nothing is waiting to be drawn (the worker is then the slower
            // side), and nothing in them shares a count with the worker. The
            // worker never touches the packet being rendered.
            static const bool s_rfree_on = [] {
                const char *e = NWII_ENV("NWII_RENDER_FREE");
                return !(e && e[0] == '0');
            }();
            bool free_here = false;
            static const bool s_latefree = [] {
                const char *e = NWII_ENV("NWII_R2_LATEFREE");
                const bool on = e && e[0] == '1';
                if (on) std::printf("[R2] NWII_R2_LATEFREE=1: commands freed after the swap\n");
                return on;
            }();
            if (s_latefree && s_rfree_on && p->render_frees) {
                // r39r2: keep the packet (s_rendering) and free it on the next call
                const int64_t d = PipeNow() - t0;
                ++s_pipe_r_n;
                const uint64_t w = p->t_ready && t0 > p->t_ready ? (uint64_t)(t0 - p->t_ready) : 0;
                s_pipe_r_wait += w;
                s_pipe_r_draw += (uint64_t)d;
                s_pipe_r_wait_max = std::max(s_pipe_r_wait_max, w);
                s_pipe_r_draw_max = std::max(s_pipe_r_draw_max, (uint64_t)d);
                s_late_pkt = p;
                return;
            }
            if (s_rfree_on && p->render_frees) {
                std::lock_guard<std::mutex> lk(s_pm);
                free_here = s_ready == nullptr && p != s_p_reading;  // r45p
            }
            // r36r NWII_R_FREECHUNK=1: free from the back in chunks of 2048
            // commands and stop as soon as the worker hands a new frame over (the
            // render thread is then the slower side): the rest is freed by the
            // worker when it reuses the packet, as without NWII_RENDER_FREE.
            static const bool s_rchunk = [] {
                const char *e = NWII_ENV("NWII_R_FREECHUNK");
                return m5_rflag(e);
            }();
            if (free_here && s_rchunk) {
                const int64_t f0 = PipeNow();
                auto &fr = p->frame;
                bool stopped = false;
                while (!fr.empty()) {
                    const size_t k = std::min<size_t>(fr.size(), 2048);
                    fr.erase(fr.end() - (ptrdiff_t)k, fr.end());
                    if (fr.empty()) break;
                    std::lock_guard<std::mutex> lk(s_pm);
                    if (s_ready) { stopped = true; break; }
                }
                if (!stopped) {
                    p->draws.clear();
                    p->entries.clear();
                    p->pack.clear();
                    p->pre.clear();
                }
                // (stopped: draws/entries/pack point into the frame or are plain
                // data; the worker clears them with what is left of the frame)
                wprof::s_rfree.fetch_add(1, std::memory_order_relaxed);
                wprof::s_rfree_ns.fetch_add((uint64_t)(PipeNow() - f0), std::memory_order_relaxed);
                free_here = false;
            }
            if (free_here) {
                const int64_t f0 = PipeNow();
                p->frame.clear();
                p->draws.clear();
                p->entries.clear();
                p->pack.clear();
                p->pre.clear();  // r36r
                wprof::s_rfree.fetch_add(1, std::memory_order_relaxed);
                wprof::s_rfree_ns.fetch_add((uint64_t)(PipeNow() - f0), std::memory_order_relaxed);
            }
        }
        {
            const uint64_t w = p->t_ready && t0 > p->t_ready ? (uint64_t)(t0 - p->t_ready) : 0;
            const uint64_t d = (uint64_t)(PipeNow() - t0);
            ++s_pipe_r_n;
            s_pipe_r_wait += w;
            s_pipe_r_draw += d;
            s_pipe_r_wait_max = std::max(s_pipe_r_wait_max, w);
            s_pipe_r_draw_max = std::max(s_pipe_r_draw_max, d);
        }
        std::lock_guard<std::mutex> lk(s_pm);
        s_rendering = nullptr;  // the worker frees its commands
        return;
    }
    static FramePacket *s_inline = new FramePacket;
    const uint64_t c0 = thread_cpu_us();
    if (!PrepFrame([](bool &) { return s_inline; }, true)) {
        g_prof_drain_cpu_us += thread_cpu_us() - c0;
        return;
    }
    RenderPacket(*s_inline, false);
    PH(5);
    s_inline->frame.clear();
    s_inline->draws.clear();
    PH(6);
    g_prof_drain_cpu_us += thread_cpu_us() - c0;
    if (PhOn() && ++g_ph_n == 120) {
        uint64_t tot = 0;
        for (auto x : g_ph) tot += x;
        printf("[R7PH] %s/frame total=%llu ", PhFd() >= 0 ? "kcyc" : "us", (unsigned long long)tot / 120);
        printf("parse=%llu carry=%llu scan=%llu split=%llu decode=%llu render=%llu destroy=%llu pre=%llu\n",
               (unsigned long long)g_ph[0]/120, (unsigned long long)g_ph[1]/120, (unsigned long long)g_ph[2]/120,
               (unsigned long long)g_ph[3]/120, (unsigned long long)g_ph[4]/120, (unsigned long long)g_ph[5]/120,
               (unsigned long long)g_ph[6]/120, (unsigned long long)g_ph[7]/120);
        for (auto &x : g_ph) x = 0;
        g_ph_n = 0;
    }
}

// ---- R8 memory report -------------------------------------------------------
// One line with what the host-side caches and buffers hold: FIFO buffers
// (bytes), parsed command vectors (capacity, what stays allocated after a
// backlog), the display-list cache, display-list snapshots, GL textures of the
// texture cache and EFB copies, and the shader programs. Host thread only (the
// thread that drains the FIFO and renders). Returns the bytes it accounts for.
void GX_RendererMemStats(size_t &programs, size_t &state_keys, size_t &cache_bytes,
                         size_t &batch_bytes);
size_t GX_MemReport(char* out, size_t n) {
    size_t hw_cap;
    {
        std::lock_guard<FifoLock> lock(g_fifo_mutex);
        hw_cap = g_hw_fifo.capacity();
    }
    // R11: the parse-side buffers and caches belong to the render worker
    // when it runs (figures it refreshes every ~2 s).
    PrepMem pm;
    if (s_worker_on.load()) {
        std::lock_guard<std::mutex> lk(s_prep_mem_m);
        pm = s_prep_mem;
    } else {
        pm = ComputePrepMem();
    }
    const size_t fifo = hw_cap + pm.parse_buf;
    const size_t cmds = pm.cmds;
    const size_t dl_n = pm.dl_n, dl_cmds = pm.dl_cmds, dl_b = pm.dl_b;
    size_t snap_n = 0, snap_b = 0;
    {
        std::lock_guard<std::mutex> lk(g_dl_snap_mtx);
        snap_n = g_dl_snap.size();
        for (const auto& kv : g_dl_snap) snap_b += 64 + kv.second->capacity();
    }
    const auto tm = nwii::runtime::hle::TextureCache::get().mem_stats();
    size_t prog = 0, keys = 0, shc = 0, batch = 0;
    GX_RendererMemStats(prog, keys, shc, batch);
    const size_t total = fifo + cmds + dl_b + snap_b + tm.tex_bytes + tm.efb_bytes + shc + batch;
    auto mb = [](size_t b) { return b / 1048576.0; };
    std::snprintf(out, n,
                  "accounted %.1f MB: fifo %.1f, cmd vectors %.1f, dlcache %zu/%zu cmds %.1f, "
                  "dlsnap %zu %.1f, tex %zu %.1f, efb %zu %.1f, programs %zu (keys %zu), "
                  "shadercache %.1f, batch %.1f; backlog waits %llu (%llu ms), trims %llu (%.1f MB)",
                  mb(total), mb(fifo), mb(cmds), dl_n, dl_cmds, mb(dl_b), snap_n, mb(snap_b),
                  tm.tex, mb(tm.tex_bytes), tm.efb, mb(tm.efb_bytes), prog, keys, mb(shc), mb(batch),
                  (unsigned long long)s_fifo_waits.load(std::memory_order_relaxed),
                  (unsigned long long)(s_fifo_wait_us.load(std::memory_order_relaxed) / 1000),
                  (unsigned long long)s_trims, mb(s_trimmed_bytes));
    return total;
}

namespace nwii::runtime {

static inline void wgp_push_debug(uint8_t b);
// Debug captures of the FIFO byte stream (NWII_GXDUMP / NWII_WGSEG /
// NWII_WGTRACE); without them a byte is just appended.
// Read at the first FIFO write (after the Switch env.txt is applied).
static inline bool wgp_debug() {
    static const bool d = NWII_ENV("NWII_GXDUMP") || NWII_ENV("NWII_WGSEG") ||
                          NWII_ENV("NWII_WGTRACE");
    return d;
}
static inline void wgp_push(uint8_t b) {
    if (__builtin_expect(wgp_debug(), 0)) wgp_push_debug(b);
    else g_hw_fifo.push_back(b);
}
static inline void wgp_push4(uint32_t v) {
    if (__builtin_expect(wgp_debug(), 0)) {
        wgp_push_debug(v >> 24); wgp_push_debug((v >> 16) & 0xFF);
        wgp_push_debug((v >> 8) & 0xFF); wgp_push_debug(v & 0xFF);
        return;
    }
    const size_t n = g_hw_fifo.size();
    g_hw_fifo.resize(n + 4);
    const uint32_t be = __builtin_bswap32(v);
    std::memcpy(g_hw_fifo.data() + n, &be, 4);
}
static inline void wgp_push_debug(uint8_t b) {

    static const char* s_dump_path = NWII_ENV("NWII_GXDUMP");
    static const char* s_seg_path = NWII_ENV("NWII_WGSEG");
    if (s_dump_path) {
        static FILE* f = fopen(s_dump_path, "wb");
        static size_t n = 0;
        if (f && n < 4000000) {
            fputc(b, f);
            fflush(f);
            ++n;
        }
    }

    
    if (const char* segp = s_seg_path) {
        static FILE* sf = fopen(segp, "w");
        static uint64_t off = 0;
        static uint32_t last_pc = 0;
        extern CPUContext* g_ctx_ptr;
        uint32_t pc = g_ctx_ptr ? g_ctx_ptr->pc : 0;
        if (sf && pc != last_pc) {
            fprintf(sf, "%llx %x\n", (unsigned long long)off, pc);
            last_pc = pc;
        }
        ++off;
    }

    
    static const bool wgtrace = NWII_ENV("NWII_WGTRACE") != nullptr;
    if (wgtrace) {
        static int zeros = 0;
        static int logged = 0;
        static uint8_t prev = 0xFF;
        static int vcd_collect = 0;      
        static uint32_t vcd_val = 0;
        static uint8_t vcd_reg = 0;
        extern CPUContext* g_ctx_ptr;
        if (vcd_collect > 0) {
            vcd_val = (vcd_val << 8) | b;
            if (--vcd_collect == 0)
                std::cout << "[WGP] VCD 0x" << std::hex << (unsigned)vcd_reg
                          << " = 0x" << vcd_val << " lr=0x"
                          << (g_ctx_ptr ? g_ctx_ptr->lr : 0) << std::dec << "\n";
        } else if (prev == 0x08 && (b == 0x50 || b == 0x60) && g_ctx_ptr &&
                   (g_ctx_ptr->pc == 0x800bbbac)) {
            vcd_collect = 4; vcd_val = 0; vcd_reg = b;
        }
        prev = b;
        if (b == 0) {
            ++zeros;
        } else {
            if (zeros >= 24 && b >= 0x80 && logged < 16 && g_ctx_ptr) {
                std::cout << "[WGP] draw 0x" << std::hex << (unsigned)b
                          << " after " << std::dec << zeros << " zeros pc=0x"
                          << std::hex << g_ctx_ptr->pc << " lr=0x"
                          << g_ctx_ptr->lr << std::dec << "\n";
                ++logged;
            }
            zeros = 0;
        }
    }
    g_hw_fifo.push_back(b);
}





static bool wgp_redirect(uint8_t b) {
    uint32_t base, end, wptr;
    nwii::runtime::hw::pi_fifo_get(base, end, wptr);
    if (base == 0) return false; 
    uint32_t cp_base = nwii::runtime::hw::cp_fifo_base_reg();
    if ((base & 0x03FFFFFF) == (cp_base & 0x03FFFFFF))
        return false; 
    if (!nwii::runtime::g_mmu) return false;
    nwii::runtime::g_mmu->write8(0x80000000u | (wptr & 0x01FFFFFFu), b);
    wptr += 1;
    if ((wptr & 0x03FFFFFF) > (end & 0x03FFFFFF))
        wptr = base;
    nwii::runtime::hw::pi_fifo_set_wptr(wptr);
    return true;
}

// Early GPU sync: the SDK writes a BP register as Write8(0x61) + Write32(reg<<24|v).
// Draw-done (BP 0x45) and draw-sync tokens (BP 0x47/0x48) are signalled to the PE
// the moment the CPU writes them instead of when the host thread parses the FIFO
// (up to a host frame later), which otherwise stalls every GXDrawDone/GXSetDrawSync.
// The renderer already owns a copy of everything before it, so this matches an
// infinitely fast GPU. NWII_LATE_SYNC=1 restores signalling from the parser.
bool g_early_gx_sync = NWII_ENV("NWII_LATE_SYNC") == nullptr;
static bool s_bp_armed = false;

namespace hw { extern bool g_wgp_redirect_on; }  // hw_pi.cpp

// ---- R8 threads: write-gather buffer on the guest thread ---------------------
// Every GXPosition/GXColor/... call wrote its 1-8 bytes straight into
// g_hw_fifo under g_fifo_mutex (an atomic exchange + vector growth per call,
// ~6% of the CPU thread). Like the real write-gather pipe, the bytes now
// collect in a small buffer of the guest thread and go into g_hw_fifo in one
// piece. Order and content of the stream are unchanged; only the moment the
// host can see the bytes moves, and the host takes the FIFO at arbitrary
// moments anyway (1 ms polls), so every stream the host can observe now is
// one it could already observe before.
//
// R16w: when the buffer is handed over. r13 handed it over 4 bytes after any
// 0x61 byte (every possible BP command): ~200 000-280 000 hand-overs per
// second of 13-15 bytes in the title demo, each one a lock + vector insert
// (wg_flush_now: 1.2 % of the CPU thread on the console, plus the byte-wise
// path for every word holding a 0x61 byte). Nothing on the host needs the
// bytes that early: it only parses what a 1 ms poll finds, the vertex arrays
// are read when the frame end is parsed, and the display-list and LOAD_INDX
// copies are made at call time (above). Now the buffer (4 KB) is handed over:
//   - when it is full;
//   - at the BPs the host or the guest's frame pacing must see at once:
//     0x52 (EFB/XFB copy: the XFB copy ends a game frame and is counted for
//     FifoWaitForHost here), 0x45 (draw done) and 0x47/0x48 (draw-sync
//     tokens, which NWII_LATE_SYNC signals from the parser) -- written as the
//     SDK does, Write8(0x61) + Write32;
//   - when the buffer is older than NWII_WGP_MAXAGE_US (default 500 us of
//     host time, checked at the guest's clock refreshes), so the worker still
//     parses a frame while the game builds it;
//   - when the guest idles (idle_wait) and after a CP FIFO drain.
// NWII_WGP_FLUSH_ALL_BP=1 also hands it over after every SDK BP write
// (close to r13). NWII_WGP_DIRECT=1 restores the direct per-write path. The
// debug captures (NWII_GXDUMP/NWII_WGSEG/NWII_WGTRACE) always use the direct
// path.
// The redirect test (FIFO pointed at a display list buffer in RAM) is a flag
// kept by the PI/CP register writes instead of two calls per write.
// Guest thread only (all GX_WGPIPE_* callers are: MMIO writes, GX seams, the
// CP FIFO drain of an MMIO write).
static constexpr uint32_t kWgCap = 4096;
alignas(64) static uint8_t s_wg[kWgCap];
static uint32_t s_wg_n = 0;
static uint32_t s_wg_cap = kWgCap;  // NWII_WGP_CAP (test: a smaller buffer)
// R16w: -1 not decided yet, 0 every write takes the full path (direct path,
// NWII_WGP_CHECK, NWII_SAMPLE), 1 the inline paths below may be used.
static int s_wg_mode = -1;
static bool s_wg_flush_all_bp = false;
// wgp_gather() / NWII_SAMPLE as plain flags once decided (function-local
// statics cost an acquire load of their guard on every call: ~1/3 of the full
// path's samples on the console).
static bool s_wg_gather = true, s_wg_sample = false;
// Counters: plain on the guest thread, published at each hand-over.
static uint64_t s_wg_writes_l = 0, s_wg_flushes_l = 0, s_wg_bytes_l = 0, s_wg_bpf_l = 0,
                s_wg_agef_l = 0;
static std::atomic<uint64_t> s_wg_writes{0}, s_wg_flushes{0}, s_wg_bytes{0}, s_wg_bpf{0},
    s_wg_agef{0};

static inline bool wgp_gather() {
    static const bool g = !wgp_debug() && NWII_ENV("NWII_WGP_DIRECT") == nullptr;
    return g;
}

// NWII_WGP_CHECK=<prefix> (debug, PC: read at start-up): every gathered write
// is also recorded, at the moment it is made, as the direct path would have
// pushed it (<prefix>.w), and every hand-over to g_hw_fifo is recorded too
// (<prefix>.f); the two files (first 512 MB) must be equal up to the bytes
// still buffered at exit. It also checks the cached redirect flag against the
// full register test and, after each write, that no frame-end copy is left
// in the buffer ([WGP] check lines).
static FILE *wg_check_open(const char *ext) {
    const char *p = NWII_ENV("NWII_WGP_CHECK");
    if (!p) return nullptr;
    char path[1024];
    std::snprintf(path, sizeof path, "%s.%s", p, ext);
    return std::fopen(path, "wb");
}
static FILE *s_wcheck = wg_check_open("w");    // the writes, as made
static FILE *s_wcheck_f = wg_check_open("f");  // what the flushes handed over
static size_t s_wcheck_n = 0, s_wcheck_fn = 0;
static uint64_t s_wcheck_bad = 0, s_wcheck_calls = 0;
static NWII_NOINLINE void wg_check(uint64_t v, int k) {
    uint8_t b[8];
    for (int i = 0; i < k; ++i) b[i] = (v >> ((k - 1 - i) * 8)) & 0xFF;
    if (s_wcheck_n < (512u << 20)) {
        std::fwrite(b, 1, k, s_wcheck);
        s_wcheck_n += k;
    }
    ++s_wcheck_calls;
}
// Called before the write: the cached redirect flag must match the full test.
static uint64_t s_wcheck_redir = 0, s_wcheck_redir_bad = 0;
static NWII_NOINLINE void wg_check_redirect() {
    uint32_t base, end, wptr;
    nwii::runtime::hw::pi_fifo_get(base, end, wptr);
    const uint32_t cp_base = nwii::runtime::hw::cp_fifo_base_reg();
    const bool on = base != 0 && (base & 0x03FFFFFF) != (cp_base & 0x03FFFFFF);
    if (on) ++s_wcheck_redir;
    if (on != hw::g_wgp_redirect_on && s_wcheck_redir_bad++ < 8)
        std::printf("[WGP] check: redirect flag %d, registers say %d\n",
                    (int)hw::g_wgp_redirect_on, (int)on);
}
// R8 memory + threads: an XFB copy (BP 0x52, bit 14) in the buffer is counted
// in s_fifo_frames by the flush that hands it over, under the same lock, as
// the direct path counts it when it pushes it; s_wg_full asks the writer to
// wait for the host afterwards (FifoWaitForHost).
static bool s_wg_xfb = false, s_wg_full = false;

// Called after the write: an XFB copy must never wait in the buffer.
static NWII_NOINLINE void wg_check_after() {
    if (s_wg_xfb && s_wcheck_bad++ < 8)
        std::printf("[WGP] check: XFB copy left in the buffer (%u bytes)\n", s_wg_n);
    if ((s_wcheck_calls & 0xFFFFFF) == 0) {
        std::printf("[WGP] check: %llu writes, %zu bytes recorded, %llu rule violations, "
                    "%llu redirected writes, %llu redirect flag mismatches\n",
                    (unsigned long long)s_wcheck_calls, s_wcheck_n,
                    (unsigned long long)s_wcheck_bad, (unsigned long long)s_wcheck_redir,
                    (unsigned long long)s_wcheck_redir_bad);
        std::fflush(s_wcheck);
        std::fflush(s_wcheck_f);
    }
}

static NWII_NOINLINE void wg_flush_now() {
    {
        std::lock_guard<FifoLock> lock(g_fifo_mutex);
        g_hw_fifo.insert(g_hw_fifo.end(), s_wg, s_wg + s_wg_n);
        if (s_wg_xfb) {
            s_wg_xfb = false;
            PipeNoteXfb();
            const uint32_t max = FifoMaxFrames();
            s_wg_full = ++s_fifo_frames + s_held_frames >= max && max != 0;
        }
    }
    if (__builtin_expect(s_wcheck_f != nullptr, 0) && s_wcheck_fn < (512u << 20)) {
        std::fwrite(s_wg, 1, s_wg_n, s_wcheck_f);
        s_wcheck_fn += s_wg_n;
    }
    ++s_wg_flushes_l;
    s_wg_bytes_l += s_wg_n;
    s_wg_flushes.store(s_wg_flushes_l, std::memory_order_relaxed);
    s_wg_bytes.store(s_wg_bytes_l, std::memory_order_relaxed);
    s_wg_writes.store(s_wg_writes_l, std::memory_order_relaxed);
    s_wg_bpf.store(s_wg_bpf_l, std::memory_order_relaxed);
    s_wg_agef.store(s_wg_agef_l, std::memory_order_relaxed);
    s_wg_n = 0;
}

void GX_WGPIPE_Flush() {
    if (s_wg_n) wg_flush_now();
}

// R16w: the buffer's age limit, in host timebase ticks (0: off). The guest's
// clock refresh (ios.cpp poll_tb_refresh, ~200 000/s) calls
// GX_WGPIPE_AgeFlush once g_wgp_age_next is reached.
uint64_t g_wgp_age_next = 0;
static uint64_t s_wgp_age_ticks = 0;
static bool s_wgp_age_init = false;
void GX_WGPIPE_AgeFlush(uint64_t now, uint64_t tb_freq) {
    if (__builtin_expect(!s_wgp_age_init, 0)) {
        s_wgp_age_init = true;
        const char *e = NWII_ENV("NWII_WGP_MAXAGE_US");
        const uint64_t us = e ? std::strtoull(e, nullptr, 10) : 500;
        s_wgp_age_ticks = us * tb_freq / 1000000;
        if (!s_wgp_age_ticks) {
            g_wgp_age_next = ~0ull;
            return;
        }
    }
    g_wgp_age_next = now + s_wgp_age_ticks;
    if (s_wg_n && s_wg_mode > 0) {
        ++s_wg_agef_l;
        wg_flush_now();
    }
}

// [r44w] NWII_W_WGP="1": caminos cortos de Write8/16/32/F32 (mismo flujo y mismo estado que los de R16w):
// una sola bandera decidida al arrancar (write-gather en línea, búfer de 4 KB, sniffer encendido) + la redirección,
// el sniffer de Write8 por tabla cuando está en reposo (s_sn_st == 0, el caso de casi todos los bytes) y el límite
// del búfer como constante. Fuera de esas condiciones, el camino de siempre.
static bool s_w1_fast = false;
static void w1_selftest();
static constexpr uint8_t kSn8Of(unsigned b) {
    return b == 0x08 ? 1 : (b & 0xE7) == 0x20 ? 3 : b == 0x40 ? 4 : b == 0x78 ? 6 : 0;
}
struct Sn8Tab { uint8_t v[256]; constexpr Sn8Tab() : v() { for (unsigned i = 0; i < 256; ++i) v[i] = kSn8Of(i); } };
alignas(64) static constexpr Sn8Tab kSn8{};
static inline bool w1_on() { return __builtin_expect(s_w1_fast & !hw::g_wgp_redirect_on, 1); }

static NWII_NOINLINE void wg_init_mode() {
    static const bool sample = NWII_ENV("NWII_SAMPLE") != nullptr;
    if (const char *e = NWII_ENV("NWII_WGP_CAP")) {
        const unsigned long c = std::strtoul(e, nullptr, 10);
        s_wg_cap = c < 16 ? 16 : c > kWgCap ? kWgCap : (uint32_t)c;
    }
    s_wg_flush_all_bp = NWII_ENV("NWII_WGP_FLUSH_ALL_BP") != nullptr;
    s_wg_gather = wgp_gather();
    s_wg_sample = sample;
    s_wg_mode = wgp_gather() && !s_wcheck && !sample ? 1 : 0;
    s_w1_fast = std::getenv("NWII_W_WGP") != nullptr && s_wg_mode == 1 && s_wg_cap == kWgCap && s_idx_on;
    if (std::getenv("NWII_W_WGP")) std::printf("[WGP] r44w NWII_W_WGP: caminos cortos %s\n", s_w1_fast ? "SI" : "no (condiciones)");
    if (s_w1_fast && std::getenv("NWII_W_WGP_SELFTEST")) w1_selftest();
    std::printf("[WGP] write-gather: %s, buffer %u B, hand-over at full / BP 0x45 0x47 0x48 0x52%s"
                " / age / idle\n",
                !wgp_gather() ? "off (direct path)" : s_wg_mode ? "on (inline paths)" : "on (checked paths)",
                s_wg_cap, s_wg_flush_all_bp ? " / every BP" : "");
}

// Room for k more bytes.
static inline void wg_room(uint32_t k) {
    if (__builtin_expect(s_wg_n + k > s_wg_cap, 0)) wg_flush_now();
}
static inline void wg_put1(uint8_t b) {
    wg_room(1);
    s_wg[s_wg_n++] = b;
}
static inline void wg_put2(uint16_t v) {
    wg_room(2);
    const uint16_t be = __builtin_bswap16(v);
    std::memcpy(s_wg + s_wg_n, &be, 2);
    s_wg_n += 2;
}
static inline void wg_put4(uint32_t v) {
    wg_room(4);
    const uint32_t be = __builtin_bswap32(v);
    std::memcpy(s_wg + s_wg_n, &be, 4);
    s_wg_n += 4;
}
static inline void wg_put8(uint64_t v) {
    wg_room(8);
    const uint64_t be = __builtin_bswap64(v);
    std::memcpy(s_wg + s_wg_n, &be, 8);
    s_wg_n += 8;
}
// The BPs the host must see at once (see above).
static inline bool wg_bp_hands_over(uint8_t reg) {
    return reg == 0x52 || reg == 0x45 || reg == 0x47 || reg == 0x48 || s_wg_flush_all_bp;
}
// The inline paths: gathering, no debug check, no display list being built.
static inline bool wg_fast() {
    return __builtin_expect((s_wg_mode > 0) & !hw::g_wgp_redirect_on, 1);
}

// [r44w] NWII_W_AGUA_CHECK (PC): mientras g_w_cap != nullptr (con s_wg_mode = 0, así todo va por los caminos
// completos) cada escritura al pipe se apunta en la captura (tipo + valor) y NO llega al pipe ni cambia su
// estado; w_agua_check la vuelve a hacer después, una a una, por las funciones de siempre.
std::vector<uint8_t> *g_w_cap = nullptr;
static NWII_NOINLINE void w_cap(uint8_t kind, uint64_t v, int n) {
    g_w_cap->push_back(kind);
    for (int i = n - 1; i >= 0; --i) g_w_cap->push_back((uint8_t)(v >> (i * 8)));
}
static NWII_NOINLINE void wgp_write8_full(uint8_t val) {
    if (__builtin_expect(g_w_cap != nullptr, 0)) { w_cap(1, val, 1); return; }
    if (__builtin_expect(s_wg_mode < 0, 0)) wg_init_mode();
    if (__builtin_expect(s_wcheck != nullptr, 0)) wg_check_redirect();
    if (__builtin_expect(hw::g_wgp_redirect_on, 0) && wgp_redirect(val)) { s_bp_armed = false; return; }
    s_bp_armed = (val == 0x61);
    idx_sniff8(val);
    if (__builtin_expect(wgp_gather(), 1)) {
        ++s_wg_writes_l;
        if (__builtin_expect(s_wcheck != nullptr, 0)) wg_check(val, 1);
        wg_put1(val);
        if (__builtin_expect(s_wcheck != nullptr, 0)) wg_check_after();
        return;
    }
    std::lock_guard<FifoLock> lock(g_fifo_mutex);
    wgp_push(val);
}

void GX_WGPIPE_Write8(uint8_t val) {
    if (w1_on()) {
        s_bp_armed = (val == 0x61);
        if (__builtin_expect(s_sn_st == 0, 1)) { s_sn_st = kSn8.v[val]; s_sn_a = val; }
        else idx_sniff8(val);
        ++s_wg_writes_l;
        uint32_t n = s_wg_n;
        if (__builtin_expect(n >= kWgCap, 0)) { wg_flush_now(); n = 0; }
        s_wg[n] = val;
        s_wg_n = n + 1;
        return;
    }
    if (wg_fast()) {
        s_bp_armed = (val == 0x61);
        idx_sniff8(val);
        ++s_wg_writes_l;
        wg_put1(val);
        return;
    }
    wgp_write8_full(val);
}

static NWII_NOINLINE void wgp_write16_full(uint16_t val) {
    if (__builtin_expect(g_w_cap != nullptr, 0)) { w_cap(2, val, 2); return; }
    if (__builtin_expect(s_wg_mode < 0, 0)) wg_init_mode();
    if (__builtin_expect(s_wcheck != nullptr, 0)) wg_check_redirect();
    s_bp_armed = false;
    if (__builtin_expect(hw::g_wgp_redirect_on, 0) && wgp_redirect(val >> 8)) { wgp_redirect(val & 0xFF); return; }
    idx_sniff16(val);
    if (__builtin_expect(wgp_gather(), 1)) {
        ++s_wg_writes_l;
        if (__builtin_expect(s_wcheck != nullptr, 0)) wg_check(val, 2);
        wg_put2(val);
        if (__builtin_expect(s_wcheck != nullptr, 0)) wg_check_after();
        return;
    }
    std::lock_guard<FifoLock> lock(g_fifo_mutex);
    wgp_push(val >> 8);
    wgp_push(val & 0xFF);
}

static inline void w1_put4(uint32_t v) {
    uint32_t n = s_wg_n;
    if (__builtin_expect(n > kWgCap - 4, 0)) { wg_flush_now(); n = 0; }
    const uint32_t be = __builtin_bswap32(v);
    std::memcpy(s_wg + n, &be, 4);
    s_wg_n = n + 4;
}
void GX_WGPIPE_Write16(uint16_t val) {
    if (w1_on()) {
        s_bp_armed = false;
        s_sn_st = 0;
        ++s_wg_writes_l;
        uint32_t n = s_wg_n;
        if (__builtin_expect(n > kWgCap - 2, 0)) { wg_flush_now(); n = 0; }
        const uint16_t be = __builtin_bswap16(val);
        std::memcpy(s_wg + n, &be, 2);
        s_wg_n = n + 2;
        return;
    }
    if (wg_fast()) {
        s_bp_armed = false;
        idx_sniff16(val);
        ++s_wg_writes_l;
        wg_put2(val);
        return;
    }
    wgp_write16_full(val);
}

// Game frames (XFB copies) written by the guest, for the Switch profiler's
// slow-scene trigger (NWII_NX_PROFILE_SLOW_MS).
std::atomic<uint32_t> g_guest_xfb{0};

static NWII_NOINLINE void wgp_write32(uint32_t val, bool is_int) {
    if (__builtin_expect(g_w_cap != nullptr, 0)) { w_cap(is_int ? 3 : 4, val, 4); return; }
    if (__builtin_expect(s_wg_mode < 0, 0)) wg_init_mode();
    if (__builtin_expect(s_wcheck != nullptr, 0)) wg_check_redirect();
    bool bp = s_bp_armed && is_int;
    s_bp_armed = false;
    if (__builtin_expect(hw::g_wgp_redirect_on, 0) && wgp_redirect(val >> 24)) {
        wgp_redirect((val >> 16) & 0xFF);
        wgp_redirect((val >> 8) & 0xFF);
        wgp_redirect(val & 0xFF);
        return;
    }
    idx_sniff32(val, is_int);
    const uint8_t reg = val >> 24;
    const bool xfb = bp && reg == 0x52 && (val & 0x4000);
    if (xfb) {
        ++s_sn_frame;
        g_guest_xfb.fetch_add(1, std::memory_order_relaxed);
    }
    if (__builtin_expect(s_wg_sample, 0)) {
        static uint64_t n = 0;
        ++n;
        if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000 ||
            (n % 100000) == 0)
            std::cout << "[GX] WGP write32 #" << std::dec << n
                      << " fifo_bytes=" << g_hw_fifo.size() << "\n";
    }
    bool full = false;
    if (__builtin_expect(s_wg_gather, 1)) {
        ++s_wg_writes_l;
        if (__builtin_expect(s_wcheck != nullptr, 0)) wg_check(val, 4);
        wg_put4(val);
        // A BP the host must see now is handed over before the PE is
        // signalled below, as the direct path pushes it first. BP 0x52 with
        // bit 14 (the copy to the XFB that ends a game frame) is counted by
        // this flush (put first, so no earlier flush can count it).
        if (bp && wg_bp_hands_over(reg)) {
            s_wg_xfb = xfb;
            ++s_wg_bpf_l;
            wg_flush_now();
            full = s_wg_full;
            s_wg_full = false;
        }
        if (__builtin_expect(s_wcheck != nullptr, 0)) wg_check_after();
    } else {
        std::lock_guard<FifoLock> lock(g_fifo_mutex);
        wgp_push4(val);
        // BP 0x52 with bit 14: the copy to the XFB that ends a game frame.
        if (xfb) {
            PipeNoteXfb();
            const uint32_t max = FifoMaxFrames();
            full = ++s_fifo_frames + s_held_frames >= max && max != 0;
        }
    }
    if (__builtin_expect(full, 0)) FifoWaitForHost();
    if (__builtin_expect(s_v_slow_due, 0)) {
        s_v_slow_due = false;
        static const bool on = std::getenv("NWII_V_SLOWTEST") != nullptr;
        if (on) VSlowTest();
    }
    if (bp && __builtin_expect(vtl::g_on, 0) && (reg == 0x45 || reg == 0x47 || reg == 0x48))
        vtl_note(12, (uint32_t)reg << 24 | (val & 0xFFFF));
    if (bp && g_early_gx_sync) {
        if (reg == 0x45 && (val & 0xF) == 2)
            nwii::runtime::hw::pe_signal_finish();
        else if (reg == 0x47)
            nwii::runtime::hw::pe_signal_token(val & 0xFFFF, false);
        else if (reg == 0x48)
            nwii::runtime::hw::pe_signal_token(val & 0xFFFF, true);
    }
}

// A 32-bit integer write: data (no command pending) takes the inline path;
// the word after Write8(0x61) (a BP) or after a CP / LOAD_INDX / CALL_DL
// opcode takes the full one.
// R16w (w4): the two frequent command words take inline paths too, doing
// exactly what the full path does for them: a BP whose register needs no
// hand-over (no XFB count, no PE signal; ~250 000/s in the title demo) and the
// value of a CP register write (array base/stride for the LOAD_INDX copies).
void GX_WGPIPE_Write32(uint32_t val) {
    if (w1_on() && __builtin_expect(!s_bp_armed & (s_sn_st == 0), 1)) {
        ++s_wg_writes_l;
        w1_put4(val);
        return;
    }
    if (wg_fast()) {
        if (__builtin_expect(!s_bp_armed & (s_sn_st == 0), 1)) {
            ++s_wg_writes_l;
            wg_put4(val);
            return;
        }
        if (s_bp_armed & (s_sn_st == 0)) {
            if (!wg_bp_hands_over(val >> 24)) {
                s_bp_armed = false;
                ++s_wg_writes_l;
                wg_put4(val);
                return;
            }
        } else if (!s_bp_armed & (s_sn_st == 2)) {
            s_sn_st = 0;
            const uint32_t a = s_sn_a;
            if (a >= 0xAC && a <= 0xAF) s_sn_base[a - 0xAC] = val & 0x3FFFFFFF;
            else if (a >= 0xBC && a <= 0xBF) s_sn_stride[a - 0xBC] = val & 0xFF;
            ++s_wg_writes_l;
            wg_put4(val);
            return;
        }
    }
    wgp_write32(val, true);
}

void GX_WGPIPE_WriteF32(float val) {
    union { float f; uint32_t i; } u; u.f = val;
    if (w1_on()) {
        s_bp_armed = false;
        s_sn_st = 0;
        ++s_wg_writes_l;
        w1_put4(u.i);
        return;
    }
    if (wg_fast()) {
        s_bp_armed = false;
        s_sn_st = 0;  // idx_sniff32(v, false)
        ++s_wg_writes_l;
        wg_put4(u.i);
        return;
    }
    wgp_write32(u.i, false);
}

static NWII_NOINLINE void wgp_write64_full(uint64_t v) {
    if (__builtin_expect(g_w_cap != nullptr, 0)) { w_cap(5, v, 8); return; }
    if (__builtin_expect(s_wg_mode < 0, 0)) wg_init_mode();
    if (__builtin_expect(s_wcheck != nullptr, 0)) wg_check_redirect();
    s_bp_armed = false;
    if (__builtin_expect(hw::g_wgp_redirect_on, 0) && wgp_redirect((v >> 56) & 0xFF)) {
        for (int s = 48; s >= 0; s -= 8)
            wgp_redirect((v >> s) & 0xFF);
        return;
    }
    idx_sniff32((uint32_t)(v >> 32), false);
    idx_sniff32((uint32_t)v, false);
    if (__builtin_expect(wgp_gather(), 1)) {
        ++s_wg_writes_l;
        if (__builtin_expect(s_wcheck != nullptr, 0)) wg_check(v, 8);
        wg_put8(v);
        if (__builtin_expect(s_wcheck != nullptr, 0)) wg_check_after();
        return;
    }
    std::lock_guard<FifoLock> lock(g_fifo_mutex);
    for (int s = 56; s >= 0; s -= 8)
        wgp_push((v >> s) & 0xFF);
}

void GX_WGPIPE_WriteF64(double val) {
    union { double d; uint64_t i; } u; u.d = val;
    if (wg_fast()) {
        s_bp_armed = false;
        s_sn_st = 0;
        ++s_wg_writes_l;
        wg_put8(u.i);
        return;
    }
    wgp_write64_full(u.i);
}

// [r44w] frente W: entrega en bloque de la cola del renderizador nativo del agua (w_agua_cola.h).
// bytes = las escrituras en orden, big endian; tape[i] = 1 (Write8), 2 (Write16) o 4 (WriteF32).
// Camino rápido (el de las funciones en línea de R16w: write-gather, sin redirección, sin NWII_WGP_CHECK):
// deja exactamente lo que dejarían esas llamadas una a una:
//   - los bytes en el búfer (entregando al llenarse, como wg_room; solo cambia en qué byte se corta la entrega);
//   - s_wg_writes_l += nwrites;
//   - s_bp_armed = la última escritura es Write8(0x61);
//   - sniffer: Write16/WriteF32 ponen s_sn_st = 0 (sin mirar s_idx_on) y no tocan s_sn_a; Write8 pasa por
//     idx_sniff8. El estado final solo depende de la última racha de Write8 (desde s_sn_st = 0 si la precede otra
//     escritura del bloque, o desde el estado de antes si empieza el bloque) y de si hay escrituras detrás.
// Si no se puede usar el camino rápido, se repiten las llamadas una a una (idéntico por construcción).
std::atomic<uint64_t> g_w_commits{0}, g_w_commit_writes{0}, g_w_commit_slow{0};
// [r44w] comprobación: fuerza los caminos completos (0) mientras se captura; devuelve el modo anterior.
// [r44w] NWII_W_WGP_SELFTEST=1 (PC, al arrancar, antes de que el juego escriba nada): las mismas secuencias
// aleatorias de Write8/16/32/F32 (bytes sesgados a 0x61/0x08/0x20-0x38/0x40/0x78/0xAC/0xBC...) por los caminos
// cortos y por los de siempre, comparando tras CADA escritura los bytes del búfer, s_sn_st, s_sn_a, s_bp_armed y
// el contador. Evita lo que tiene efectos fuera del pipe (LOAD_INDX/CALL_DL completos, BP que entregan): esas
// palabras van por el camino completo en las dos versiones igualmente.
static void w1_selftest() {
    uint64_t rs = 0x9E3779B97F4A7C15ull, nops = 0, nbad = 0;
    auto rnd = [&]() { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return rs; };
    static const uint8_t kSpecial[] = {0x61, 0x08, 0x20, 0x28, 0x30, 0x38, 0x40, 0x78, 0xAC, 0xBC, 0x10, 0x00};
    const bool save = s_w1_fast;
    for (int seq = 0; seq < 4000; ++seq) {
        const uint64_t seed = rnd();
        std::vector<uint64_t> trace[2];
        for (int pass = 0; pass < 2; ++pass) {
            s_w1_fast = pass == 1;
            s_wg_n = 0; s_sn_st = 0; s_sn_a = 0; s_bp_armed = false;
            uint64_t r2 = seed | 1;
            auto rr = [&]() { r2 ^= r2 << 13; r2 ^= r2 >> 7; r2 ^= r2 << 17; return r2; };
            for (int op = 0; op < 200; ++op) {
                const uint64_t x = rr();
                const int kind = (int)(x % 4);
                const uint8_t sb = (x >> 8) % 3 ? kSpecial[(x >> 16) % sizeof kSpecial] : (uint8_t)(x >> 24);
                if (kind == 0) GX_WGPIPE_Write8(sb);
                else if (kind == 1) GX_WGPIPE_Write16((uint16_t)(sb << 8 | (x >> 32 & 0xFF)));
                else if (kind == 2) {
                    // sin efectos fuera del pipe: ni LOAD_INDX/CALL_DL pendientes ni BP que entregan
                    if (s_sn_st == 3 || s_sn_st == 4 || s_sn_st == 5) { GX_WGPIPE_Write16(0x1234); }
                    else {
                        uint32_t v = (uint32_t)(x >> 32);
                        const uint8_t top = v >> 24;
                        if (top == 0x45 || top == 0x47 || top == 0x48 || top == 0x52) v ^= 0x01000000u;
                        GX_WGPIPE_Write32(v);
                    }
                } else { union { uint32_t i; float f; } u; u.i = (uint32_t)(x >> 32); GX_WGPIPE_WriteF32(u.f); }
                uint64_t h = s_wg_n;
                h = h * 1000003u ^ (uint64_t)s_sn_st;
                h = h * 1000003u ^ s_sn_a;
                h = h * 1000003u ^ (uint64_t)s_bp_armed;
                h = h * 1000003u ^ s_wg_writes_l;
                for (uint32_t i = s_wg_n > 8 ? s_wg_n - 8 : 0; i < s_wg_n; ++i) h = h * 131u ^ s_wg[i];
                trace[pass].push_back(h);
                ++nops;
            }
            s_wg_writes_l -= 200;  // el contador debe sumar lo mismo en las dos pasadas; se deshace
        }
        if (trace[0] != trace[1]) ++nbad;
    }
    s_wg_n = 0; s_sn_st = 0; s_sn_a = 0; s_bp_armed = false;
    s_w1_fast = save;
    std::printf("[WGP] r44w selftest: %d secuencias, %llu escrituras por cada camino, %llu secuencias distintas -> %s\n",
                4000, (unsigned long long)(nops / 2), (unsigned long long)nbad, nbad ? "FALLO" : "OK");
}
static bool s_w1_saved = false;
int GX_W_ModeSwap(int m) {
    const int o = s_wg_mode;
    if (m <= 0) { s_w1_saved = s_w1_fast; s_w1_fast = false; }
    else s_w1_fast = s_w1_saved;
    s_wg_mode = m;
    return o;
}
void GX_WGPIPE_CommitTape(const uint8_t *b, const uint8_t *t, uint32_t nw, uint32_t nb) {
    if (!nw) return;
    if (__builtin_expect(!wg_fast() || g_w_cap != nullptr, 0)) {
        g_w_commit_slow.fetch_add(1, std::memory_order_relaxed);
        uint32_t p = 0;
        for (uint32_t i = 0; i < nw; ++i) {
            if (t[i] == 1) { GX_WGPIPE_Write8(b[p]); p += 1; }
            else if (t[i] == 2) { GX_WGPIPE_Write16((uint16_t)(b[p] << 8 | b[p + 1])); p += 2; }
            else { union { uint32_t i; float f; } u; u.i = (uint32_t)b[p] << 24 | (uint32_t)b[p + 1] << 16 |
                   (uint32_t)b[p + 2] << 8 | b[p + 3]; GX_WGPIPE_WriteF32(u.f); p += 4; }
        }
        return;
    }
    // estado final de BP/sniffer
    const uint32_t last = nw - 1;
    s_bp_armed = t[last] == 1 && b[nb - 1] == 0x61;
    int32_t e = (int32_t)last;
    uint32_t pe = nb;  // posición tras la escritura e
    while (e >= 0 && t[e] != 1) { pe -= t[e]; --e; }
    if (e < 0) {
        s_sn_st = 0;
    } else if (!s_idx_on) {
        // alguna escritura no-Write8 en el bloque -> s_sn_st = 0 (Write8 no lo toca sin s_idx_on)
        bool any = false;
        for (uint32_t i = 0; i < nw; ++i) any |= t[i] != 1;
        if (any) s_sn_st = 0;
    } else {
        int32_t s0 = e;
        while (s0 > 0 && t[s0 - 1] == 1) --s0;
        const uint32_t ps = pe - (uint32_t)(e - s0 + 1);
        if (s0 > 0) s_sn_st = 0;
        for (uint32_t p = ps; p < pe; ++p) idx_sniff8(b[p]);
        if (e != (int32_t)last) s_sn_st = 0;
    }
    s_wg_writes_l += nw;
    g_w_commits.fetch_add(1, std::memory_order_relaxed);
    g_w_commit_writes.fetch_add(nw, std::memory_order_relaxed);
    uint32_t p = 0;
    while (p < nb) {
        uint32_t room = s_wg_cap - s_wg_n;
        if (!room) { wg_flush_now(); room = s_wg_cap; }
        const uint32_t k = nb - p < room ? nb - p : room;
        std::memcpy(s_wg + s_wg_n, b + p, k);
        s_wg_n += k;
        p += k;
    }
}

// ", wgp N writes/s M flushes/s" for the NWII_FPS / fps_log stats line
// (R16w: + of them at a BP / by age).
int GX_WgpStatsLine(char *buf, int size, double dt) {
    static uint64_t w0 = 0, f0 = 0, b0 = 0, p0 = 0, a0 = 0;
    const uint64_t w = s_wg_writes.load(std::memory_order_relaxed);
    const uint64_t f = s_wg_flushes.load(std::memory_order_relaxed);
    const uint64_t b = s_wg_bytes.load(std::memory_order_relaxed);
    const uint64_t p = s_wg_bpf.load(std::memory_order_relaxed);
    const uint64_t a = s_wg_agef.load(std::memory_order_relaxed);
    int r = 0;
    if (!wgp_gather())
        r = std::snprintf(buf, size, ", wgp direct");
    else if (dt > 0)
        r = std::snprintf(buf, size, ", wgp %.0fk writes/s %.1fk flushes/s (%.0f B; bp %.1fk/s, age %.1fk/s)",
                          (w - w0) / dt / 1000, (f - f0) / dt / 1000,
                          f > f0 ? double(b - b0) / double(f - f0) : 0.0,
                          (p - p0) / dt / 1000, (a - a0) / dt / 1000);
    w0 = w; f0 = f; b0 = b; p0 = p; a0 = a;
    return r;
}

} 
uint64_t g_prof_decode_us_accum = 0;


// ---- r38c (frente C): estado para el vigilante de cuelgues (s2.cpp) ----
namespace nwii::runtime {
uint64_t c38_frames_render() { return __atomic_load_n(&g_stat_frames, __ATOMIC_RELAXED); }
uint64_t c38_frames_game() { return s_c38_xfb.load(std::memory_order_relaxed); }
void c38_stress_fifo_touch() { std::lock_guard<FifoLock> lock(g_fifo_mutex); }
void c38_gx_state(char *buf, int n) {
    const uint32_t own = g_fifo_mutex.owner.load(std::memory_order_relaxed);
    const char *who = "?";
#ifdef __SWITCH__
    static const char *const nm[] = {"main", "cpu", "ax", "aud", "gxw", "snd"};
    for (int i = 0; i < 6; ++i)
        if (own && own == nwii::runtime::nx::thread_handle(i)) who = nm[i];
#endif
    std::snprintf(buf, n, "fifo candado %d (ultimo dueno %s 0x%x, esperan %u) fot en cola %u retenidos %u bytes %zu",
                  (int)g_fifo_mutex.busy.load(), who, own, g_fifo_mutex.waiters.load(), s_fifo_frames, s_held_frames,
                  g_hw_fifo.size());
}
}  // namespace nwii::runtime
