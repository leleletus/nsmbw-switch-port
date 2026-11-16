// sysclk_client.h: minimal client for the sys-clk sysmodule ("sys:clk") and
// its Horizon OC fork ("hoc:clk"): asks for a temporary CPU frequency
// override while the game runs (m6r). See sysclk_client.cpp.
#pragma once

namespace nwii::runtime::nx {

// config.toml [switch] cpu_mhz (-> NWII_NX_CPU_MHZ; default 1224, 0 = off)
// and NWII_NX_SYSCLK_OFF=1. Does nothing (and never fails) when no sys-clk
// is installed or any call fails. Logs one "[NX] sys-clk: ..." line.
void sysclk_start();
// m6s: also an applet hook: the override is removed when the game loses focus
// (HOME, sleep, an applet on top) and asked again when it gets it back, because
// HOME -> close kills the process without running shutdown()/atexit.
// Removes the override (idempotent; also registered with atexit).
void sysclk_stop();

}  // namespace nwii::runtime::nx
