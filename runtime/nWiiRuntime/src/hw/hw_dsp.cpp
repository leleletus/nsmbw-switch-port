#include "runtime/nwii_env.h"
#include "runtime/hw/hw.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include "../hle/ax/ax_hle.h"
#include "../core/s2.h"  // r34s2
#include <atomic>
#include <thread>
#include <iostream>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <deque>
#include <mutex>
#include <vector>

namespace nwii::runtime {
    extern MMU* g_mmu;
    extern CPUContext *g_ctx_ptr;
    extern uint64_t get_os_time();
}

#include "runtime/event_scheduler.h"

// Mailbox registers are polled in tight loops; read the switch once.
static const bool g_dsp_trace = NWII_ENV("NWII_DSPTRACE") != nullptr;
// NWII_AX_LEGACY=1: only acknowledge AX command lists (no audio), as before.
static const bool g_ax_legacy = NWII_ENV("NWII_AX_LEGACY") != nullptr;

namespace nwii::runtime::hw {

static uint16_t dsp_mbox_cpu_hi = 0;
static uint16_t dsp_mbox_cpu_lo = 0;
static uint16_t dsp_mbox_dsp_hi = 0;
static uint16_t dsp_mbox_dsp_lo = 0;
static uint32_t ar_mmaddr = 0;
static uint32_t ar_araddr = 0;
static uint32_t ar_cnt = 0;
// DSPCSR bit layout (Dolphin UDSPControl): RES=0x0001, PIINT=0x0002,



static constexpr uint16_t CSR_INT_BITS = 0x00A8;
static uint16_t dsp_control = 0x0800;
static uint16_t ar_size = 0;
static uint16_t ar_mode = 0;
static uint16_t ar_refresh = 0;



static void dsp_update_pi();

// ---- r34s2 (NWII_S2): el DSP atendido por dos hilos ------------------------------------------
// Con la CPU de sonido en marcha (s_route_snd), las interrupciones AID (0x08) y DSP (0x80) y todos
// los eventos de tiempo del DSP (fin de pasada del DMA de audio, correos) son suyos: van a su
// agenda (s2::snd_schedule_at) y no levantan la interrupcion PI del hilo del juego, que solo ve la
// de ARAM (0x20). Todo el estado de este archivo se toca bajo s_dsp_lock (recursivo por hilo).
static std::atomic<bool> s_dsp_lock{false};
static thread_local int t_dsp_depth = 0;
struct DspGuard {
    const bool on;
    DspGuard() : on(nwii::runtime::s2::g_enabled) {
        if (on && t_dsp_depth++ == 0) {
            bool exp = false;
            unsigned spins = 0;
            uint32_t spins32 = 0;  // r38c: gira poco y luego duerme (s2::spin_backoff)
            while (!s_dsp_lock.compare_exchange_weak(exp, true, std::memory_order_acquire, std::memory_order_relaxed)) {
                exp = false;
                nwii::runtime::s2::spin_backoff(spins32);
            }
            (void)spins;
        }
    }
    ~DspGuard() {
        if (on && --t_dsp_depth == 0) s_dsp_lock.store(false, std::memory_order_release);
    }
};
static bool s_route_snd = false;               // bajo s_dsp_lock
static std::atomic<uint32_t> s_snd_pend{0};    // interrupciones de sonido pendientes (0x08 | 0x80)
static int s_ev_pending = 0;                   // eventos de correo en vuelo (no cuenta el del DMA de audio)
using DspCb = std::function<void(CPUContext &, uint64_t)>;
static uint64_t dsp_now() {
    return s_route_snd ? nwii::runtime::s2::snd_now() : EventScheduler::get().now();
}
static DspCb dsp_wrap(DspCb cb, bool count) {
    return [cb = std::move(cb), count](CPUContext &c, uint64_t late) {
        DspGuard g;
        if (count) --s_ev_pending;
        cb(c, late);
    };
}
static uint64_t dsp_sched(uint64_t delay, DspCb cb, bool count = true) {
    if (count) ++s_ev_pending;
    if (s_route_snd) {
        if (nwii::runtime::s2::on_snd_thread())
            return nwii::runtime::s2::snd_schedule_at(nwii::runtime::s2::snd_now() + delay, dsp_wrap(std::move(cb), count));
        static int warned = 0;
        if (warned++ < 5) std::printf("[S2] AVISO: evento del DSP programado desde el hilo del juego con la CPU de sonido en marcha\n");
    }
    return EventScheduler::get().schedule_after(delay, dsp_wrap(std::move(cb), count));
}



static void ar_dma_execute() {
    bool dir = (ar_cnt & 0x80000000) != 0;
    uint32_t count = ar_cnt & 0x7FFFFFFF;
    uint32_t mm = ar_mmaddr & 0x01FFFFFF;
    uint32_t ar_a = ar_araddr & 0x00FFFFFF;
    if (g_mmu && count > 0) {
        uint8_t *mem1 = nwii::runtime::g_mmu->mem1.data();
        uint8_t *mem2 = nwii::runtime::g_mmu->mem2.data();
        uint32_t mem1_size = nwii::runtime::g_mmu->mem1.size();
        uint32_t mem2_size = nwii::runtime::g_mmu->mem2.size();
        if (mm < mem1_size && ar_a < mem2_size) {
            uint32_t copy_count = std::min({count, mem1_size - mm, mem2_size - ar_a});
            if (!dir) std::memcpy(mem2 + ar_a, mem1 + mm, copy_count);
            else std::memcpy(mem1 + mm, mem2 + ar_a, copy_count);
        }
    }





    if (nwii::runtime::s2::g_enabled && nwii::runtime::s2::on_snd_thread()) {
        ar_cnt = 0;  // r34s2: la agenda del juego no es de este hilo (no deberia pasar: ARAM lo usa el juego)
        dsp_control |= 0x0020;
        dsp_update_pi();
        return;
    }
    EventScheduler::get().schedule_after(
        (uint64_t)count / 2 + 64, [](CPUContext&, uint64_t) {
            DspGuard g;
            ar_cnt = 0;
            dsp_control |= 0x0020;
            dsp_update_pi();
        });
}

// ---- DSP -> CPU mailbox ----
// The microcode can post several mails back to back (DSP_SYNC, then DSP_YIELD
// once a command list is done). Only the front one is visible; reading its low
// half pops it and the next one raises the DSP interrupt again.
static std::deque<uint32_t> s_dsp_mails;

static void dsp_present_next_mail() {
    if ((dsp_mbox_dsp_hi & 0x8000) || s_dsp_mails.empty())
        return;
    const uint32_t mail = s_dsp_mails.front();
    s_dsp_mails.pop_front();
    dsp_mbox_dsp_hi = (uint16_t)(mail >> 16);
    dsp_mbox_dsp_lo = (uint16_t)mail;
    dsp_control |= 0x0080;
    dsp_update_pi();
}

// `coalesce`: drop the mail if an identical one is still unread, which is what
// the old single-slot mailbox did for the per-mail boot acknowledgements.
static void dsp_post_mail(uint32_t mail, uint64_t delay, bool coalesce = false) {
    dsp_sched(delay, [mail, coalesce](CPUContext&, uint64_t) {
        if (coalesce) {
            const bool visible = (dsp_mbox_dsp_hi & 0x8000) &&
                                 (((uint32_t)dsp_mbox_dsp_hi << 16) | dsp_mbox_dsp_lo) == mail;
            if (visible || std::find(s_dsp_mails.begin(), s_dsp_mails.end(), mail) !=
                               s_dsp_mails.end())
                return;
        }
        s_dsp_mails.push_back(mail);
        dsp_present_next_mail();
    });
}

// ---- AX command lists (R8: may mix on the AX thread, see ax_async.cpp) ----
// DSP_SYNC ("frame mixed") goes out 0.1 ms of guest time after the list mail,
// DSP_YIELD ("list done") 1 ms after it, as in r7b. With the AX thread
// (default, NWII_AX_THREAD=2) the SYNC event re-checks every 25 us until the
// mix is done, like the real DSP, which answers when it has mixed the frame;
// it is never later than the YIELD, which always follows it. =1 makes the
// SYNC event wait for the mix at exactly 0.1 ms instead.
static bool s_ax_sync_pending = false;
static uint64_t s_ax_seq = 0;  // lists submitted (the events below belong to one)

static void dsp_push_mail_now(uint32_t mail) {
    s_dsp_mails.push_back(mail);
    dsp_present_next_mail();
}

static void ax_deliver_sync() {
    s_ax_sync_pending = false;
    if (ax::finish())
        dsp_push_mail_now(0xDCD10004u);  // DSP_SYNC: the list produced a frame
}

static void ax_sync_check(uint64_t seq) {
    if (!s_ax_sync_pending || seq != s_ax_seq)
        return;
    static const bool slide = ax::sync_slides();  // one parser: ax_async.cpp opts()
    if (slide && !ax::done()) {
        dsp_sched(
            g_ctx_ptr->tb_freq / 40000, [seq](CPUContext &, uint64_t) { ax_sync_check(seq); });
        return;
    }
    ax_deliver_sync();
}

static void ax_submit_list(uint32_t addr, uint16_t size) {
    const uint64_t ms = g_ctx_ptr->tb_freq / 1000;
    if (!ax::async_enabled()) {  // inline: exactly the r7b sequence
        ax::submit(addr, size);
        if (ax::finish())
            dsp_post_mail(0xDCD10004u, ms / 10);  // DSP_SYNC
        dsp_post_mail(0xDCD10002u, ms);           // DSP_YIELD
        return;
    }
    // r34s2: en la CPU de sonido la lista se mezcla en el propio hilo, en el correo (orden de r7b). Con el
    // hilo AX aparte (mismo nucleo y prioridad, sin reparto de tiempo) la mezcla caia DESPUES o EN MEDIO de
    // la vuelta del SoundThread y leia bloques de voz a medio preparar: mezclas de 61/122/183 ms en la consola.
    static const bool s2_inline = [] { const char *e = NWII_ENV("NWII_S2_AXINLINE"); return !(e && *e == '0'); }();
    if (s_route_snd && s2_inline && nwii::runtime::s2::on_snd_thread()) {
        if (s_ax_sync_pending)
            ax_deliver_sync();
        if (ax::mix_inline(addr, size))
            dsp_post_mail(0xDCD10004u, ms / 10);  // DSP_SYNC
        dsp_post_mail(0xDCD10002u, ms);           // DSP_YIELD
        return;
    }
    if (s_ax_sync_pending)
        ax_deliver_sync();  // overload: a new list before the last one's SYNC
    ax::submit(addr, size);
    s_ax_sync_pending = true;
    const uint64_t seq = ++s_ax_seq;
    dsp_sched(
        ms / 10, [seq](CPUContext &, uint64_t) { ax_sync_check(seq); });
    dsp_sched(ms, [seq](CPUContext &, uint64_t) {
        if (s_ax_sync_pending && seq == s_ax_seq)
            ax_deliver_sync();  // the SYNC always comes before its YIELD
        dsp_push_mail_now(0xDCD10002u);  // DSP_YIELD
    });
}

// ---- Audio output ----
// AX writes each 3 ms frame as interleaved R,L 16-bit big-endian samples;
// audio DMA plays them at 32 kHz. Played frames go into a ring the host audio
// device drains (dsp_audio_pull), optionally mirrored to a WAV file
// (NWII_AUDIODUMP=<file.wav>).
static constexpr uint32_t kAudioRate = 32000;
static constexpr size_t kRingFrames = 1u << 15;  // ~1 s
static int16_t s_ring[kRingFrames * 2];
static size_t s_ring_read = 0, s_ring_write = 0;  // monotonic frame counters
static std::mutex s_ring_mutex;

static std::FILE *s_wav = nullptr;
static uint32_t s_wav_frames = 0;

static void wav_put32(std::FILE *f, uint32_t v) {
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    std::fwrite(b, 1, 4, f);
}

static void wav_write_header() {
    const uint32_t data = s_wav_frames * 4;
    std::fseek(s_wav, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, s_wav);
    wav_put32(s_wav, 36 + data);
    std::fwrite("WAVEfmt ", 1, 8, s_wav);
    wav_put32(s_wav, 16);
    wav_put32(s_wav, 1 | (2u << 16));          // PCM, 2 channels
    wav_put32(s_wav, kAudioRate);
    wav_put32(s_wav, kAudioRate * 4);           // byte rate
    wav_put32(s_wav, 4 | (16u << 16));          // block align, bits per sample
    std::fwrite("data", 1, 4, s_wav);
    wav_put32(s_wav, data);
    std::fseek(s_wav, 0, SEEK_END);
}

static void wav_append(const int16_t *lr, size_t frames) {
    static const char *path = NWII_ENV("NWII_AUDIODUMP");
    if (!path)
        return;
    if (!s_wav) {
        s_wav = std::fopen(path, "wb");
        if (!s_wav) {
            path = nullptr;
            return;
        }
        wav_write_header();
    }
    for (size_t i = 0; i < frames * 2; ++i) {
        const uint16_t v = (uint16_t)lr[i];
        const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
        std::fwrite(b, 1, 2, s_wav);
    }
    const uint32_t before = s_wav_frames;
    s_wav_frames += (uint32_t)frames;
    // Keep the header valid so a killed run still leaves a playable file.
    if (before / kAudioRate != s_wav_frames / kAudioRate) {
        wav_write_header();
        std::fflush(s_wav);
    }
}

static void audio_push_from_ram(uint32_t addr, uint32_t frames) {
    if (!g_mmu || frames == 0)
        return;
    std::vector<int16_t> lr(frames * 2);
    for (uint32_t i = 0; i < frames; ++i) {
        const uint8_t *p = g_mmu->get_ptr((addr + i * 4) | 0x80000000);
        const int16_t r = p ? (int16_t)((p[0] << 8) | p[1]) : 0;
        const int16_t l = p ? (int16_t)((p[2] << 8) | p[3]) : 0;
        lr[i * 2] = l;
        lr[i * 2 + 1] = r;
    }
    wav_append(lr.data(), frames);
    std::lock_guard<std::mutex> lock(s_ring_mutex);
    for (uint32_t i = 0; i < frames; ++i) {
        if (s_ring_write - s_ring_read >= kRingFrames)
            ++s_ring_read;  // consumer fell behind: drop the oldest frame
        const size_t slot = (s_ring_write++ & (kRingFrames - 1)) * 2;
        s_ring[slot] = lr[i * 2];
        s_ring[slot + 1] = lr[i * 2 + 1];
    }
}

size_t dsp_audio_available() {
    std::lock_guard<std::mutex> lock(s_ring_mutex);
    return s_ring_write - s_ring_read;
}

// Copies up to `frames` stereo L,R frames; returns how many were available.
size_t dsp_audio_pull(int16_t* out, size_t frames) {
    std::lock_guard<std::mutex> lock(s_ring_mutex);
    const size_t n = std::min(frames, s_ring_write - s_ring_read);
    for (size_t i = 0; i < n; ++i) {
        const size_t slot = (s_ring_read++ & (kRingFrames - 1)) * 2;
        out[i * 2] = s_ring[slot];
        out[i * 2 + 1] = s_ring[slot + 1];
    }
    return n;
}

// ---- Audio DMA ----
// One pass plays NumBlocks x 32 bytes (8 stereo frames per block) from the
// latched address. At the end of a pass the DMA reloads address/length from
// the registers and raises AID; the AX library programs the next buffer from
// that interrupt. For AX one pass is one frame: 12 blocks, 96 frames, 3 ms.
static uint32_t adma_addr = 0;
static uint16_t adma_control = 0;
static uint64_t s_adma_event_id = 0;
static bool s_adma_on_snd = false;  // r34s2: en que agenda esta ese evento
static uint32_t s_adma_pass_blocks = 0;
static uint64_t s_adma_pass_start = 0;
// With the guest clock tied to wall time, a host stall can let two passes end
// before the CPU services AID, and hardware would replay the old buffer. A
// buffer the CPU has not re-armed since it was latched is not played again (a
// short gap the host queue absorbs beats an audible repeat), unless the CPU
// stops re-arming altogether.
static bool s_adma_rearmed = true;
static int s_adma_stale_passes = 0;

static void adma_event_cb(CPUContext& ctx, uint64_t late);
static void dsp_cancel(uint64_t id) {
    if (s_adma_on_snd) {
        if (nwii::runtime::s2::on_snd_thread()) nwii::runtime::s2::snd_cancel(id, nullptr);
        else std::printf("[S2] AVISO: el hilo del juego cancela el DMA de audio con la CPU de sonido en marcha\n");
    } else {
        EventScheduler::get().cancel(id);
    }
}

// r31t (frente T): NWII_ADMA_ABS=1 -> el fin de cada pasada se programa desde la HORA TEORICA
// del fin de la anterior (y no desde el instante en que el hilo del juego la atendio), asi
// el retraso del sondeo no se acumula y el audio sale a 32 kHz justos aunque el sondeo sea
// mas espaciado (NWII_POLL_EVERY). Si el retraso llega a una pasada entera (parada del
// anfitrion) se programa desde ahora, como siempre.
static uint64_t s_adma_late = 0;  // retraso con que se atendio el fin de la pasada anterior
static bool adma_abs_on() {
    static const bool on = [] { const char *e = NWII_ENV("NWII_ADMA_ABS"); return e && *e && *e != '0'; }();
    return on;
}

// r38c NWII_C_CATCHUP=<n pasadas> (CPU de sonido): si el fin de pasada se atiende tarde, la hora TEORICA se conserva
// hasta n pasadas de retraso (antes: solo hasta 1; con mas, se reprogramaba desde ahora y se perdian ticks -> AID < 333/s
// cuando la CPU de sonido va por debajo del trabajador de render). Las pasadas atrasadas se recuperan seguidas, pero
// separadas al menos NWII_C_CATCHUP_GAP_US (1200: el correo DSP_YIELD de la lista anterior llega a 1 ms).
static uint64_t s_c38_theo_next = 0;  // hora teorica (reloj de sonido) del siguiente fin de pasada; 0 = no se sigue
static uint64_t s_c38_caught = 0, s_c38_resets = 0;
static uint32_t c38_catchup() {
    static const uint32_t n = [] { const char *e = NWII_ENV("NWII_C_CATCHUP"); return e ? (uint32_t)std::atoi(e) : 0u; }();
    return n;
}
static uint64_t c38_gap_ticks() {
    static const uint64_t us = [] { const char *e = NWII_ENV("NWII_C_CATCHUP_GAP_US"); return e ? (uint64_t)std::atol(e) : 1200ull; }();
    return us * g_ctx_ptr->tb_freq / 1000000;
}
void c38_dsp_catchup_stats(uint64_t *caught, uint64_t *resets) { *caught = s_c38_caught; *resets = s_c38_resets; }

static void adma_start_pass() {
    s_adma_pass_blocks = adma_control & 0x7FFF;
    s_adma_pass_start = dsp_now();
    if (s_adma_rearmed || ++s_adma_stale_passes > 8)
        audio_push_from_ram(adma_addr, s_adma_pass_blocks * 8);
    if (s_adma_rearmed)
        s_adma_stale_passes = 0;
    s_adma_rearmed = false;
    const uint32_t blocks = std::max<uint32_t>(s_adma_pass_blocks, 1);
    uint64_t ticks = (uint64_t)blocks * 8 * g_ctx_ptr->tb_freq / kAudioRate;
    // r34s2: en la CPU de sonido siempre desde la hora teorica (32 kHz justos, 333,3 listas/s).
    const uint32_t cn = c38_catchup();
    if (cn && s_route_snd) {  // r38c
        const uint64_t now = dsp_now();
        if (s_adma_late < (uint64_t)cn * ticks) {
            const uint64_t theo = now - s_adma_late + ticks;
            const uint64_t gap = c38_gap_ticks();
            const uint64_t due = std::max(theo, now + std::min(gap, ticks));
            if (due > theo) ++s_c38_caught;
            s_c38_theo_next = theo;
            ticks = due - now;
        } else {
            if (s_c38_theo_next) ++s_c38_resets;
            s_c38_theo_next = now + ticks;
        }
    } else {
        s_c38_theo_next = 0;
        if ((adma_abs_on() || s_route_snd) && s_adma_late < ticks) ticks -= s_adma_late;
    }
    s_adma_late = 0;
    s_adma_on_snd = s_route_snd && nwii::runtime::s2::on_snd_thread();
    s_adma_event_id = dsp_sched(ticks, adma_event_cb, false);
}

static void adma_event_cb(CPUContext& ctx, uint64_t late) {
    s_adma_event_id = 0;
    if (!(adma_control & 0x8000))
        return;
    if (s_c38_theo_next && s_route_snd) {  // r38c: retraso contra la hora teorica (no contra la programada)
        const uint64_t now = dsp_now();
        late = now > s_c38_theo_next ? now - s_c38_theo_next : 0;
    }
    s_adma_late = late;
    adma_start_pass();
    dsp_control |= 0x0008;
    dsp_update_pi();
}

static uint16_t adma_blocks_left() {
    if (!(adma_control & 0x8000) || !g_ctx_ptr)
        return 0;
    const uint64_t elapsed = dsp_now() - s_adma_pass_start;
    const uint64_t done = elapsed * kAudioRate / (8 * g_ctx_ptr->tb_freq);
    const uint64_t left = done < s_adma_pass_blocks ? s_adma_pass_blocks - done : 0;
    return (uint16_t)(left > 0 ? left - 1 : 0);  // zero-based, like hardware
}

// three DSP subsystem sources — Dolphin's (csr >> 1) & csr trick.
static void dsp_update_pi() {
    uint16_t pend = (dsp_control >> 1) & dsp_control & CSR_INT_BITS;
    if (s_route_snd) {  // r34s2: AID y DSP son de la CPU de sonido
        s_snd_pend.store(pend & 0x0088, std::memory_order_release);
        pend &= 0x0020;
    }
    if (pend)
        trigger_pi_interrupt(0x40);
    else
        clear_pi_interrupt(0x40);
}

// ---- r34s2: toma / devolucion del DSP (hilo del juego, CPU de sonido aparcada) ----
static bool dsp_quiescent_locked() {
    return s_ev_pending == 0 && !s_ax_sync_pending && s_dsp_mails.empty() &&
           (((dsp_control >> 1) & dsp_control & 0x0088) == 0);
}
bool c38_dsp_lock_busy() { return s_dsp_lock.load(std::memory_order_relaxed); }  // r38c: vigia
bool s2_dsp_quiescent() {
    DspGuard g;
    return dsp_quiescent_locked();
}
int s2_dsp_irq() {
    const uint32_t p = s_snd_pend.load(std::memory_order_acquire);
    return (p & 0x08) ? 5 : (p & 0x80) ? 7 : 0;  // mismo orden que __OSDispatchInterrupt (numero mas bajo primero)
}
bool s2_dsp_takeover() {
    DspGuard g;
    if (!dsp_quiescent_locked() || !(adma_control & 0x8000) || s_adma_event_id == 0 || !g_ctx_ptr) return false;
    const uint64_t now = EventScheduler::get().now();
    const uint64_t pass = (uint64_t)std::max<uint32_t>(s_adma_pass_blocks, 1) * 8 * g_ctx_ptr->tb_freq / kAudioRate;
    const uint64_t elapsed = now > s_adma_pass_start ? now - s_adma_pass_start : 0;
    const uint64_t remaining = elapsed < pass ? pass - elapsed : 0;
    if (!EventScheduler::get().cancel(s_adma_event_id)) return false;
    s_route_snd = true;
    const uint64_t snow = nwii::runtime::s2::snd_now();
    s_adma_pass_start = snow - elapsed;
    s_adma_late = 0;
    s_adma_on_snd = true;
    s_adma_event_id = nwii::runtime::s2::snd_schedule_at(snow + remaining, dsp_wrap(adma_event_cb, false));
    dsp_update_pi();
    return true;
}
void s2_dsp_handback() {
    DspGuard g;
    if (!s_route_snd) return;
    uint64_t due = 0;
    const uint64_t snow = nwii::runtime::s2::snd_now();
    const bool had = s_adma_event_id != 0 && s_adma_on_snd && nwii::runtime::s2::snd_cancel(s_adma_event_id, &due);
    const uint64_t elapsed = snow > s_adma_pass_start ? snow - s_adma_pass_start : 0;
    s_route_snd = false;
    s_adma_on_snd = false;
    s_snd_pend.store(0, std::memory_order_release);
    if (had) {
        const uint64_t now = EventScheduler::get().now();
        s_adma_pass_start = now > elapsed ? now - elapsed : 0;
        s_adma_event_id = EventScheduler::get().schedule_after(due > snow ? due - snow : 0, dsp_wrap(adma_event_cb, false));
    }
    dsp_update_pi();
}





int dsp_pending_os_interrupt() {
    DspGuard g;
    if (s_route_snd) return 6;  // r34s2: al hilo del juego solo le llega ARAM
    uint16_t pending = (dsp_control >> 1) & dsp_control & CSR_INT_BITS;
    if (pending & 0x0080) return 7;
    if (pending & 0x0020) return 6;
    if (pending & 0x0008) return 5;
    return 7;
}



void register_dsp(MMIODispatcher& dispatcher) {{
    dispatcher.register_region(0xCC005000, 0xCC0050FF,
        [](uint32_t addr) -> uint32_t {
            DspGuard g;
            if (addr == 0xCC005000 || addr == 0xCC005002) return dsp_mbox_cpu_hi;
            if (addr == 0xCC005004) return dsp_mbox_dsp_hi;
            if (addr == 0xCC005006) {
                uint16_t val = dsp_mbox_dsp_lo;
                dsp_mbox_dsp_hi &= ~0x8000;



                dsp_control &= ~0x0080;
                dsp_update_pi();
                if (g_dsp_trace)
                    std::cout << "[DSPm] mail read lo=0x" << std::hex << val
                              << " (hi was 0x" << (dsp_mbox_dsp_hi | 0x8000)
                              << ") csr=0x" << dsp_control << std::dec << "\n";
                dsp_present_next_mail();
                return val;
            }
            if (addr == 0xCC005008 || addr == 0xCC00500A) {
                if (s_route_snd)  // r34s2: cada CPU ve solo sus interrupciones
                    return dsp_control & (nwii::runtime::s2::on_snd_thread() ? ~0x0020u : ~0x0088u);
                return dsp_control;
            }
            if (addr == 0xCC005012) return ar_size;


            if (addr == 0xCC005016) return ar_mode | 0x0001;
            if (addr == 0xCC00501A) return ar_refresh;
            if (addr == 0xCC005020) return ar_mmaddr;
            if (addr == 0xCC005024) return ar_araddr;
            if (addr == 0xCC005028) return ar_cnt;
            if (addr == 0xCC005030) return adma_addr >> 16;
            if (addr == 0xCC005032) return adma_addr & 0xFFFF;
            if (addr == 0xCC005036) return adma_control;
            if (addr == 0xCC00503A) return adma_blocks_left();
            return 0;
        },
        [](uint32_t addr, uint32_t val) {
            DspGuard g;
            static const bool artrace = NWII_ENV("NWII_ARTRACE") != nullptr;
            if (artrace && (addr & 0xFFF0) >= 0x5010 && (addr & 0xFFF0) < 0x5030)
                std::cout << "[ARw] 0x" << std::hex << (addr & 0xFFFF)
                          << " = 0x" << val << std::dec << "\n";
            if (addr == 0xCC005000 || addr == 0xCC005002) {
                // CPU -> DSP mail, complete once the low half is written. The
                // AX microcode protocol (Dolphin AXUCode::HandleMail):
                // 0xBABE<size>, then the command list address -> run the list,
                // post DSP_SYNC (frame output) and DSP_YIELD (list done); the
                // CPU then answers with a 0xCDD1xxxx task mail.
                static uint16_t s_cpu_mail_hi = 0;
                static int s_ax_state = 0;  // 1: got 0xBABE, 2: list done
                static uint16_t s_cmdlist_size = 0;
                bool completed = (addr == 0xCC005002);
                if (addr == 0xCC005000)
                    s_cpu_mail_hi = val & 0xFFFF;
                dsp_mbox_cpu_hi = (val & 0x7FFF) | 0x8000;
                dsp_mbox_cpu_hi &= ~0x8000;
                if (completed && (dsp_control & 0x0100)) {
                    uint32_t mail = ((uint32_t)s_cpu_mail_hi << 16) | (val & 0xFFFF);
                    const uint64_t ms = g_ctx_ptr->tb_freq / 1000;
                    auto reply = [ms](uint16_t lo) { dsp_post_mail(0xDCD10000u | lo, ms); };
                    if (s_ax_state == 1) {
                        if (!g_ax_legacy)
                            ax_submit_list(mail, s_cmdlist_size);  // SYNC + YIELD
                        else
                            reply(0x0002);  // DSP_YIELD
                        s_ax_state = 2;
                    } else if ((mail & 0xFFFF0000u) == 0xBABE0000u) {
                        s_cmdlist_size = mail & 0xFFFF;
                        s_ax_state = 1;
                    } else if (mail == 0xCDD10001u) {  // MAIL_NEW_UCODE
                        reply(0x0001);
                        s_ax_state = 0;
                    } else if ((mail & 0xFFFF0000u) == 0xCDD10000u) {
                        s_ax_state = 0;
                        if (g_ax_legacy)
                            reply(0x0000);
                        else if (mail == 0xCDD10000u)  // MAIL_RESUME
                            reply(0x0001);              // DSP_RESUME
                        // MAIL_CONTINUE / MAIL_RESET: no acknowledgement.
                    } else {
                        dsp_post_mail(0xDCD10000u, ms, true);
                    }
                }
                if (g_dsp_trace)
                    std::cout << "[DSPm] cpu mail write @" << std::hex
                              << (addr & 0xFFFF) << " val=0x" << val
                              << " csr=0x" << dsp_control << std::dec << "\n";
            } else if (addr == 0xCC005004 || addr == 0xCC005006) {
                if (val & 0x8000) dsp_mbox_dsp_hi &= ~0x8000;
            } else if (addr == 0xCC005008 || addr == 0xCC00500A) {
                uint16_t val16 = val & 0xFFFF;
                if (g_dsp_trace)
                    std::cout << "[DSPm] csr write val=0x" << std::hex
                              << (val & 0xFFFF) << " csr=0x" << dsp_control
                              << std::dec << "\n";


                uint16_t kept_ints = dsp_control & CSR_INT_BITS & ~val16;
                bool was_halted = (dsp_control & 0x0800) != 0;
                bool is_halted = (val16 & 0x0800) != 0;
                dsp_control = kept_ints | (val16 & ~(CSR_INT_BITS | 0x0001));





                if ((val16 & 0x0001) || (was_halted && !is_halted)) {
                    s_dsp_mails.clear();
                    dsp_mbox_dsp_hi = 0xDCD1;
                    dsp_mbox_dsp_lo = 0x0000;
                }
                dsp_update_pi();
            } else if (addr == 0xCC005012) {
                ar_size = val & 0xFFFF;
            } else if (addr == 0xCC005016) {
                ar_mode = val & 0xFFFF;
            } else if (addr == 0xCC00501A) {
                ar_refresh = val & 0xFFFF;
            } else if (addr == 0xCC005030) {
                adma_addr = (adma_addr & 0x0000FFFF) | ((val & 0xFFFF) << 16);
                s_adma_rearmed = true;
            } else if (addr == 0xCC005032) {
                adma_addr = (adma_addr & 0xFFFF0000) | (val & 0xFFFF);
                s_adma_rearmed = true;
            } else if (addr == 0xCC005036) {
                bool was_on = (adma_control & 0x8000) != 0;
                adma_control = val & 0xFFFF;
                // A running DMA picks up new values when its pass ends.
                if (!was_on && (adma_control & 0x8000)) {
                    if (s_adma_event_id != 0)
                        dsp_cancel(s_adma_event_id);
                    adma_start_pass();
                } else if (was_on && !(adma_control & 0x8000)) {
                    if (s_adma_event_id != 0) {
                        dsp_cancel(s_adma_event_id);
                        s_adma_event_id = 0;
                    }
                }
            } else if (addr == 0xCC005020) {
                ar_mmaddr = val;
            } else if (addr == 0xCC005024) {
                ar_araddr = val;
            } else if (addr == 0xCC005028) {
                ar_cnt = val;
                ar_dma_execute();
            }
        },

        // (Dolphin DSP.cpp: MMADDR_H/L, ARADDR_H/L, CNT_H/L) and the SDK's




        [](uint32_t addr) -> uint16_t {
            DspGuard g;
            if (addr == 0xCC005020) return (uint16_t)(ar_mmaddr >> 16);
            if (addr == 0xCC005022) return (uint16_t)ar_mmaddr;
            if (addr == 0xCC005024) return (uint16_t)(ar_araddr >> 16);
            if (addr == 0xCC005026) return (uint16_t)ar_araddr;
            if (addr == 0xCC005028) return (uint16_t)(ar_cnt >> 16);
            if (addr == 0xCC00502A) return (uint16_t)ar_cnt;
            return (uint16_t)MMIODispatcher::get().read32(addr);
        },
        [](uint32_t addr, uint16_t val) {
            DspGuard g;
            static const bool artrace = NWII_ENV("NWII_ARTRACE") != nullptr;
            if (artrace && (addr & 0xFFFF) >= 0x5020 && (addr & 0xFFFF) < 0x5030)
                std::cout << "[ARw16] 0x" << std::hex << (addr & 0xFFFF)
                          << " = 0x" << val << std::dec << "\n";
            if      (addr == 0xCC005020) ar_mmaddr = (ar_mmaddr & 0x0000FFFF) | ((uint32_t)val << 16);
            else if (addr == 0xCC005022) ar_mmaddr = (ar_mmaddr & 0xFFFF0000) | val;
            else if (addr == 0xCC005024) ar_araddr = (ar_araddr & 0x0000FFFF) | ((uint32_t)val << 16);
            else if (addr == 0xCC005026) ar_araddr = (ar_araddr & 0xFFFF0000) | val;
            else if (addr == 0xCC005028) ar_cnt = (ar_cnt & 0x0000FFFF) | ((uint32_t)val << 16);
            else if (addr == 0xCC00502A) {
                ar_cnt = (ar_cnt & 0xFFFF0000) | val;
                ar_dma_execute();
            }
            else MMIODispatcher::get().write32(addr, val);
        }
    );
}}

void dsp_trigger_interrupt() {
    DspGuard g;
    if (dsp_control & 0x0100) {
        dsp_control |= 0x0080;
        trigger_pi_interrupt(0x40);
    }
}

}
