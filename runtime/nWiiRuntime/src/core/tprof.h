// r31t (frente T): perfil del hilo del juego con la PILA INVITADA de cada muestra.
// NWII_TPROF=1 lo enciende (solo depuracion). Ver tprof.cpp.
#pragma once
#include <cstdint>

namespace nwii::runtime {
struct CPUContext;
namespace tprof {
// True si NWII_TPROF esta puesto. Reserva las tablas la primera vez.
bool init(const char *log_dir, uint64_t image_base, const char *build);
// Una muestra del hilo del juego. Se llama con ese hilo detenido (Switch: desde
// el muestreador, entre pausa y reanudacion; PC: desde su manejador de senal).
// No usa malloc ni stdio. host_ret: direcciones de retorno del anfitrion
// (rebuscadas en su pila) o nullptr.
void sample(const CPUContext &c, uint64_t host_pc, uint64_t host_lr, uint32_t t_us,
            const uint64_t *host_ret, int n_host_ret);
// Muestra de otro hilo del anfitrion (render = 0, trabajador gxw = 4...): pc/lr y, si
// espera en el nucleo, sus retornos. Mismo hilo muestreador que sample().
void sample_host(int slot, uint64_t host_pc, uint64_t host_lr, uint32_t t_us,
                 const uint64_t *host_ret, int n_host_ret);
// Anade lo nuevo a logs/tprof_*.bin (fuera de la pausa).
void flush();
// PC (Linux): temporizador + senal para el hilo que llama (el de la CPU).
void attach_cpu_thread();
}  // namespace tprof
}  // namespace nwii::runtime
