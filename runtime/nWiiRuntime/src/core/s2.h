// r34s2 (frente S2): el sonido fuera del hilo del juego ("CPU de sonido").
// NWII_S2=1 (apagado por defecto). Diseno y razonamiento de correccion: r9/r34s2/LEEME.md.
//
// Un segundo CPUContext (misma memoria invitada) ejecutado por un hilo anfitrion propio corre:
//   - las interrupciones del DSP que son de sonido (AID = fin de pasada del DMA de audio, cada 3 ms,
//     y DSP = correos del microcodigo AX) con los manejadores del propio juego (__AIDHandler, __DSPHandler);
//   - el cuerpo del hilo nw4r::snd::SoundThread (bucle de 0x8027B990: OSReceiveMessage -> FrameProcess).
// El hilo invitado SoundThread de verdad se queda dormido en su cola para siempre (nadie le manda mensajes).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>

namespace nwii::runtime {
struct CPUContext;
namespace s2 {

extern bool g_enabled;                 // NWII_S2=1 al arrancar
extern std::atomic<bool> g_active;     // la CPU de sonido esta en marcha (es duena de AID/DSP y del SoundThread)
extern std::atomic<uint32_t> g_req;    // 1 = peticion de la CPU de sonido pendiente para la CPU del juego
extern std::atomic<uint32_t> g_game_work;  // la CPU del juego tiene algo que mirar en su sondeo lento
bool on_snd_thread();                  // este hilo anfitrion es la CPU de sonido

void init(CPUContext *main_ctx);       // main(), antes de arrancar el hilo de la CPU
void shutdown();
// --- CPU del juego (ios.cpp / os_hle_stubs.cpp) ---
void game_poll_slow(CPUContext &ctx);  // sondeo lento: peticiones, toma y devolucion del sonido
inline void game_poll(CPUContext &ctx) {
  if (__builtin_expect(g_game_work.load(std::memory_order_relaxed) | g_req.load(std::memory_order_relaxed), 0))
    game_poll_slow(ctx);
}
void game_irq_enter(CPUContext &ctx);  // justo antes de entregar una interrupcion / callback al invitado
void game_sync(CPUContext &ctx);       // tras cargar un contexto desde el runtime
void game_idle_sleep(uint64_t us);     // siesta del bucle ocioso, interrumpible
// --- CPU de sonido ---
bool snd_poll(CPUContext &ctx);        // sondeo lento del contexto de sonido (devuelve false)
CPUContext *snd_ctx();
// --- lado DSP (hw_dsp.cpp): agenda propia de la CPU de sonido ---
using Cb = std::function<void(CPUContext &, uint64_t)>;
uint64_t snd_now();
uint64_t snd_schedule_at(uint64_t due, Cb cb);
bool snd_cancel(uint64_t id, uint64_t *due);
void stats_line(char *buf, int n, double dt);
// --- r38c (frente C) ---
void spin_backoff(uint32_t &spins);    // espera activa: gira poco y luego DUERME (cede el nucleo a los de abajo)
void vigia_start(CPUContext *main_ctx);
// PC: NWII_C_PCSCHED=1 imita el reparto de la Switch (nucleo fijo + SCHED_FIFO con las prioridades de Horizon:
// mayor prioridad = numero menor). slot: 0 render, 1 juego, 4 trabajador, 5 CPU de sonido. En la Switch, nada.
void pc_sched(int slot);
bool pc_set_prio(int horizon_prio);  // PC: cambia la del hilo actual

}  // namespace s2

namespace hw {
// hw_dsp.cpp. Las dos primeras las llama el hilo del juego con la CPU de sonido aparcada.
bool s2_dsp_takeover();   // mueve el evento del DMA de audio a la agenda de sonido; false si el DSP no esta en reposo
void s2_dsp_handback();   // lo devuelve a la agenda del juego
int s2_dsp_irq();         // CPU de sonido: 5 (AID), 7 (DSP) o 0
bool s2_dsp_quiescent();  // sin eventos de correo en vuelo ni interrupciones de sonido pendientes
bool c38_dsp_lock_busy();  // r38c: vigia
void c38_dsp_catchup_stats(uint64_t *caught, uint64_t *resets);  // r38c: NWII_C_CATCHUP
}  // namespace hw
// r38c: vigia (gx.cpp / plataforma)
uint64_t c38_frames_render();
uint64_t c38_frames_game();
void c38_gx_state(char *buf, int n);
void c38_log_flush();
void c38_stress_fifo_touch();  // gx.cpp: toma y suelta g_fifo_mutex (solo pruebas)
}  // namespace nwii::runtime
