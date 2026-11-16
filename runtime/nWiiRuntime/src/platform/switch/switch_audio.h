#pragma once
// Native Switch audio output (libnx audren/audrv), used instead of SDL's
// audio device on the NRO. It runs on its own thread (core 2, priority 0x2B)
// and pulls the frames the DSP played straight from the DSP output ring
// (hw_dsp.cpp dsp_audio_pull), so audio no longer depends on the rate of the
// host (render) loop. debug.toml NWII_NX_AUDIO = "sdl" goes back to SDL.
#include <cstddef>

namespace nwii::runtime::nx {

// Starts the output thread. Returns false (nothing started) when
// NWII_NX_AUDIO=sdl or audren could not be opened: the caller then opens the
// SDL audio device as before.
bool audio_start();

// Stops the thread and closes audren (no-op when not started).
void audio_stop();

// fps_log: ", audio nx q <ms> (ring <ms>), underruns ..." (empty when off).
void audio_report(char *buf, size_t n, double dt);

}  // namespace nwii::runtime::nx
