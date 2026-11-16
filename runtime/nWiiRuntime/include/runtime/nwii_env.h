#pragma once
#include <cstdlib>

// Debug switches are read from the environment once per call site: several sit
// on per-instruction paths (interrupt polling, GX FIFO writes), where a raw
// getenv() scan cost ~7% of the CPU thread.
#define NWII_ENV(name) ([]() -> const char * { static const char *v = std::getenv(name); return v; }())

