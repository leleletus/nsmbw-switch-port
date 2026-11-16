#pragma once
// Switch (libnx) process setup for the NRO build, called from main.cpp under
// __SWITCH__. Everything lives in the NRO's folder (e.g. sdmc:/switch/NSMBWnx/):
//   config.toml    player settings, written with defaults when missing
//   debug.toml     developers only, optional: fps_log, cpu_boost, NWII_* ...
//   data/          the user's extracted disc (sys/ and files/)
//   save/          emulated NAND (saves)
//   cache/         shadercache.bin
//   logs/          created on the first error (nwii_err.log) or debug log (nwii.log)
// Older layouts still work (SMNE01/, nsmbw.toml, nand/, env.txt, sdmc:/nsmbw/).
// Production defaults: real-time clock (NWII_REALTIME=1), no per-frame log
// output, language from the console settings.

namespace nwii::runtime::nx {

// First thing in main(): default argv, env.txt, log sinks, sanity checks of
// the game files and memory (with an on-screen message when something is
// wrong). Returns false when the user chose to quit from such a message.
bool early_init(int *argc, char ***argv);

// First thing on the recompiled-CPU thread: move it to a core of its own
// (the main thread renders on core 0).
void cpu_thread_start();
// Last thing on the CPU thread.
void cpu_thread_finished();
// True once cpu_thread_finished() ran.
bool cpu_thread_done();

// R8: the calling thread is one of the threads fps_log reports CPU time for
// (0 main/render, 1 recompiled CPU, 2 AX audio mixer, 3 audio output, 4 render
// worker: GX FIFO parse + decode, R11).
enum ThreadSlot { kThreadMain = 0, kThreadCpu = 1, kThreadAx = 2, kThreadAudio = 3, kThreadGxw = 4,
                  kThreadS2 = 5 /* r34s2: CPU de sonido */, kThreadSlots = 6 };
void note_thread(int slot);
// Kernel handle of a noted thread (0 until it ran note_thread).
unsigned thread_handle(int slot);
void overlay_thread_load(float *render, float *game);  // r10: fps overlay
// The NRO's folder (sdmc:/switch/NSMBWnx), where logs/ lives.
const char *root_dir();

// Once per host frame: periodic log flush and (with NWII_LOG) fps/memory stats.
void frame_tick();

// r38c: flush stdout + log file to the SD (hang watchdog).
void c38_sink_flush();

// At exit: stop rumble, flush and close logs.
void shutdown();

}  // namespace nwii::runtime::nx
