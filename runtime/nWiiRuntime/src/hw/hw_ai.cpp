#include "runtime/hw/hw.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include <iostream>
#include <cstring>
#include <algorithm>

namespace nwii::runtime {
    extern uint64_t get_os_time();
}

namespace nwii::runtime::hw {

// AI control register bits, as Dolphin's AudioInterface names them.
constexpr uint32_t AICR_PSTAT    = 0x01;  // playing: sample counter runs
constexpr uint32_t AICR_AISFR    = 0x02;  // streaming rate: 1 = 48 kHz, 0 = 32 kHz
constexpr uint32_t AICR_AIINTMSK = 0x04;  // raise the PI interrupt on AIINT
constexpr uint32_t AICR_AIINT    = 0x08;  // counter reached AIIT (write 1 to clear)
constexpr uint32_t AICR_AIINTVLD = 0x10;  // 1 = AIIT matches do not set AIINT
constexpr uint32_t AICR_SCRESET  = 0x20;  // write 1: reset the sample counter
constexpr uint32_t AICR_AIDFR    = 0x40;  // Wii DSP rate select
constexpr uint32_t AICR_WRITABLE =
    AICR_PSTAT | AICR_AISFR | AICR_AIINTMSK | AICR_AIINTVLD | AICR_AIDFR;
constexpr uint32_t PI_INT_AI = 0x20;

static uint32_t ai_cr = 0;
static uint32_t ai_vr = 0;
static uint32_t ai_scnt = 0;        // counter value when ai_start_time was taken
static uint32_t ai_it = 0;
static uint32_t ai_last_scnt = 0;   // last counter value checked against AIIT
static uint64_t ai_start_time = 0;  // microseconds

static uint32_t ai_counter() {
    if (!(ai_cr & AICR_PSTAT)) return ai_scnt;
    uint64_t rate = (ai_cr & AICR_AISFR) ? 48000 : 32000;
    uint64_t elapsed = nwii::runtime::get_os_time() - ai_start_time;
    return ai_scnt + (uint32_t)(elapsed * rate / 1000000);
}

// AIINT is raised when the counter crosses AIIT, not every time it is past it,
// so clearing AIINT does not immediately re-raise it.
static void ai_check_interrupt() {
    uint32_t cur = ai_counter();
    bool crossed = ai_it != 0 && ai_last_scnt < ai_it && cur >= ai_it;
    ai_last_scnt = cur;
    if (crossed && !(ai_cr & AICR_AIINTVLD)) {
        ai_cr |= AICR_AIINT;
        if (ai_cr & AICR_AIINTMSK) trigger_pi_interrupt(PI_INT_AI);
    }
}

static void ai_restart_counter(uint32_t value) {
    ai_scnt = value;
    ai_last_scnt = value;
    ai_start_time = nwii::runtime::get_os_time();
}

static void register_ai_at(MMIODispatcher& dispatcher, uint32_t base) {
    dispatcher.register_region(base, base + 0xFF,
        [base](uint32_t addr) -> uint32_t {
            ai_check_interrupt();
            switch (addr - base) {
            case 0x00: return ai_cr;
            case 0x04: return ai_vr;
            case 0x08: return ai_counter();
            case 0x0C: return ai_it;
            default: return 0;
            }
        },
        [base](uint32_t addr, uint32_t val) {
            switch (addr - base) {
            case 0x00: {
                if (val & AICR_AIINT) {
                    ai_cr &= ~AICR_AIINT;
                    clear_pi_interrupt(PI_INT_AI);
                }
                bool was_playing = ai_cr & AICR_PSTAT;
                bool playing = val & AICR_PSTAT;
                uint32_t cur = ai_counter();
                ai_cr = (ai_cr & AICR_AIINT) | (val & AICR_WRITABLE);
                if (val & AICR_SCRESET) ai_restart_counter(0);
                else if (playing != was_playing) ai_restart_counter(cur);
                break;
            }
            case 0x04: ai_vr = val; break;
            case 0x08: ai_restart_counter(val); break;
            case 0x0C: ai_it = val; break;
            }
        }
    );
}

void register_ai(MMIODispatcher& dispatcher) {
    // GameCube address and the Hollywood mirror the Wii SDK uses.
    register_ai_at(dispatcher, 0xCC006C00);
    register_ai_at(dispatcher, 0xCD006C00);
}

void ai_update() {
    ai_check_interrupt();
}

}
