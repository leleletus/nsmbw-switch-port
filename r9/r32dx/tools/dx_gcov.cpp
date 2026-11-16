// dx_gcov.cpp (r32dx): solo se enlaza en el binario de PC con gcov. Da a dx.cpp el volcado por segmento.
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
extern "C" void __gcov_dump(void);
extern "C" void __gcov_reset(void);
namespace nwii::runtime {
extern void (*g_dx_gcov_flush)(const char *dir);
static void flush(const char *dir) {
  mkdir(dir, 0755);
  setenv("GCOV_PREFIX", dir, 1);
  setenv("GCOV_PREFIX_STRIP", "99", 1);   // sin carpetas: <dir>/output_N.gcda
  __gcov_dump();
  __gcov_reset();
}
static struct Init { Init() { g_dx_gcov_flush = flush; setenv("GCOV_PREFIX", "/tmp/dx_gcov_resto", 0); setenv("GCOV_PREFIX_STRIP", "99", 0); } } s_init;
}  // namespace nwii::runtime
