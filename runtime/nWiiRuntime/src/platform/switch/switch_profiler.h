#pragma once
// Debug only: sampling profiler for real hardware. debug.toml `profile = true`
// (NWII_NX_PROFILE=1) starts a sampler thread that, every NWII_NX_PROFILE_US
// (1000) microseconds, pauses the main (render) and the recompiled-CPU
// threads for an instant (svcSetThreadActivity + svcGetThreadContext3), takes
// their host pc/lr (and the guest PPC pc of the CPU context), and every
// NWII_NX_PROFILE_SECS (10) seconds writes the histograms to
// logs/profile_<thread>.txt with the image base, for
// tools/nx_profile_symbolize.py and the matching .elf.

namespace nwii::runtime::nx {

// No-op unless NWII_NX_PROFILE is set. Call once logging is set up.
void profiler_start();

}  // namespace nwii::runtime::nx
