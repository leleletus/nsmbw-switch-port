#ifdef __SWITCH__
#include "../../core/s2.h"  // r34s2
#include "switch_platform.h"
#include "switch_input.h"
#include "switch_audio.h"
#include "switch_profiler.h"

#include <switch.h>
#include <sys/iosupport.h>
#include <toml++/toml.hpp>
#include <sys/stat.h>
#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <unistd.h>
#include <string>
#include <vector>

// R8 memory report of the host caches (hle/gx.cpp).
size_t GX_MemReport(char *out, size_t n);

extern uint64_t g_stat_frames;  // gx.cpp: XFB copies (game frames) so far
extern "C" char __end__[];  // libnx switch.ld: end of the loaded image
namespace nwii::runtime {
void governor_report(char *buf, size_t n, double dt);  // os_hle_stubs.cpp
}

namespace nwii::runtime::nx {

namespace {

// Everything lives in the NRO's own folder (e.g. sdmc:/switch/NSMBWnx/ for
// sdmc:/switch/NSMBWnx/NSMBWnx.nro):
//   config.toml  settings (written with defaults when missing)
//   data/        the user's extracted disc (sys/ and files/)
//   save/        emulated NAND (Config::load default)
//   cache/       shadercache.bin        logs/  nwii.log, nwii_err.log
// Older layouts still work: SMNE01/ instead of data/, nsmbw.toml instead of
// config.toml, nand/ instead of save/, and the whole sdmc:/nsmbw/ folder when
// the game files are only there.
constexpr const char *kLegacyRoot = "sdmc:/nsmbw";
std::string g_root;
std::string g_game_dir, g_config, g_env_file;

// Written when the config file is missing. [hle] sched_lock_offset is read at
// run time and the game hangs without it.
constexpr const char *kDefaultToml =
    "# NSMBWnx: configuración (si borras este archivo, se vuelve a crear así)\n"
    "#\n"
    "# Carpeta del juego (todo junto al .nro):\n"
    "#   data/   tu disco NSMBW USA (SMNE01) extraído: data/sys y data/files\n"
    "#   save/   partidas guardadas (cópiala para tener respaldo)\n"
    "#   cache/  gráficos ya preparados (se puede borrar)\n"
    "#   logs/   registros para reportar fallos\n"
    "\n"
    "[system]\n"
    "language = \"auto\"        # auto = idioma de la consola, o \"es\" / \"en\" / \"fr\"\n"
    "\n"
    "[input]\n"
    "players = 4               # cuántos jugadores pueden conectarse (1-4)\n"
    "joycon_mode = \"auto\"      # auto = J1 con par de Joy-Con y J2-4 con uno suelto;\n"
    "                          # \"single\" = cada Joy-Con un jugador; \"pair\" = L+R juntos\n"
    "nx_controller_applet = true  # abrir el applet de mandos en \"Añadir control\"\n"
    "mode = 6                  # no tocar\n"
    "\n"
    "[switch]\n"
    "motion = true             # inclinar/sacudir girando el mando (false = solo botones)\n"
    "tilt_invert = false       # true = inclinación al revés\n"
    "\n"
    "[graphics]\n"
    "# Resolución interna: \"auto\" = \"720p\" en portátil y \"1080p\" en el dock (la imagen del\n"
    "# juego sale 1:1 en la pantalla). 1 = la de la Wii (640x528, lo más ligero); también vale\n"
    "# cualquier número entre 1 y 4 (\"1.3636\", \"1.5\", 2...) o \"720p\"/\"1080p\". Más alto = más\n"
    "# nítido y más trabajo de GPU. Un valor aquí vale para los dos modos.\n"
    "internal_resolution = \"auto\"\n"
    "# Solo en modo portátil (quítale el # para usarla): por ejemplo 1 si la GPU no llega.\n"
    "#internal_resolution_handheld = 1\n"
    "backend = \"opengl\"        # no tocar\n"
    "window_width = 1280       # no tocar\n"
    "window_height = 720       # no tocar\n"
    "vsync = true              # no tocar\n"
    "\n"
    "# ---- no tocar ----\n"
    "[game]\n"
    "id = \"SMNE01\"\n"
    "\n"
    "[hle]\n"
    "sched_lock_offset = 0x4F80   # necesario: sin esto el juego se cuelga\n";

// ---------------------------------------------------------------- logging
// stdout/stderr (fds 1 and 2) go through these devoptabs into files on the SD
// card. The files are opened on the first write, written through a large
// buffer that frame_tick() flushes every few seconds, and capped in size so a
// log storm can neither fill the card nor stall the game on SD writes.
struct LogSink {
  std::string path;
  size_t cap;
  bool flush_each_write;
  FILE *f = nullptr;
  size_t written = 0;
  bool failed = false;
  std::mutex m;
  std::vector<char> buf;

  // r41m (frente M) NWII_M_LOGBG=1: los escritores solo copian a `pend` bajo `m`; un hilo de fondo de prioridad
  // baja (logbg_main) intercambia `pend` por `wbuf` y hace fwrite/fflush/fsync SIN `m` (con `fm`, que solo
  // toman el hilo de fondo, el vaciado síncrono del vigía y el cierre). Sin el interruptor: como antes.
  bool bg = false;
  std::vector<char> pend;  // bajo m
  std::vector<char> wbuf;  // bajo fm
  std::mutex fm;
  size_t pend_drop = 0;    // bajo m: bytes tirados porque el hilo de fondo no daba abasto
  static constexpr size_t kPendMax = 8u << 20;
  // r41m: medidas ([LOGF]): tiempo de pared de write() (espera del mutex incluida)
  std::atomic<uint64_t> st_w_n{0}, st_w_max_ticks{0};

  ssize_t write(const char *ptr, size_t len) {
    const uint64_t t0 = armGetSystemTick();
    struct Note {
      LogSink &s; uint64_t t0;
      ~Note() {
        const uint64_t d = armGetSystemTick() - t0;
        s.st_w_n.fetch_add(1, std::memory_order_relaxed);
        if (d > s.st_w_max_ticks.load(std::memory_order_relaxed)) s.st_w_max_ticks.store(d, std::memory_order_relaxed);
      }
    } note{*this, t0};
    std::lock_guard<std::mutex> lk(m);
    if (failed || written >= cap) return (ssize_t)len;  // dropped, not an error
    if (bg) {
      const size_t n = std::min(len, cap - written);
      if (pend.size() + n > kPendMax) { pend_drop += n; return (ssize_t)len; }
      pend.insert(pend.end(), ptr, ptr + n);
      written += n;
      if (written >= cap) {
        static const char kLim[] = "\n[NX] log size limit reached, later output dropped\n";
        pend.insert(pend.end(), kLim, kLim + sizeof kLim - 1);
      }
      return (ssize_t)len;
    }
    if (!f) {
      // logs/ only appears once something is logged (errors, or a debug log).
      mkdir(path.substr(0, path.rfind('/')).c_str(), 0777);
      f = std::fopen(path.c_str(), "w");
      if (!f) {
        failed = true;
        return (ssize_t)len;
      }
      buf.resize(256 * 1024);
      setvbuf(f, buf.data(), _IOFBF, buf.size());
    }
    const size_t n = std::min(len, cap - written);
    std::fwrite(ptr, 1, n, f);
    written += n;
    if (written >= cap)
      std::fputs("\n[NX] log size limit reached, later output dropped\n", f);
    if (flush_each_write || written >= cap) {
      std::fflush(f);
      fsync(fileno(f));
    }
    return (ssize_t)len;
  }
  // r41m NWII_M_LOGBG: lleva `pend` a la SD (fwrite + fflush + fsync) sin tener `m` durante la escritura.
  // La llaman el hilo de fondo, flush() (vigía, frame_tick) y close(). Devuelve los bytes escritos.
  size_t drain() {
    std::lock_guard<std::mutex> lf(fm);
    {
      std::lock_guard<std::mutex> lk(m);
      if (!f && failed) return 0;
      std::swap(pend, wbuf);
    }
    if (wbuf.empty()) return 0;
    if (!f) {
      mkdir(path.substr(0, path.rfind('/')).c_str(), 0777);
      FILE *nf = std::fopen(path.c_str(), "w");
      if (!nf) {
        std::lock_guard<std::mutex> lk(m);
        failed = true;
        wbuf.clear();
        return 0;
      }
      buf.resize(256 * 1024);
      setvbuf(nf, buf.data(), _IOFBF, buf.size());
      std::lock_guard<std::mutex> lk(m);
      f = nf;
    }
    const size_t n = wbuf.size();
    std::fwrite(wbuf.data(), 1, n, f);
    std::fflush(f);
    fsync(fileno(f));
    wbuf.clear();
    return n;
  }
  void flush() {
    if (bg) { drain(); return; }
    std::lock_guard<std::mutex> lk(m);
    if (!f) return;
    std::fflush(f);
    // The SD's FAT entry (file size) is only committed on close or fsFileFlush: without this, a game
    // closed from HOME (process killed, no fclose) leaves a 0-byte log.
    fsync(fileno(f));
  }
  void close() {
    if (bg) drain();
    std::lock_guard<std::mutex> lf(fm);
    std::lock_guard<std::mutex> lk(m);
    if (f) std::fclose(f);
    f = nullptr;
    failed = true;  // late writes (static destructors) are dropped
  }
};

LogSink g_out_sink{"", 64u << 20, false};  // paths set by choose_root()
LogSink g_err_sink{"", 1u << 20, true};

// r41m (frente M) NWII_M_LOGBG=1: hilo de fondo del registro (prioridad 0x3B, por debajo de render 0x2C, sonido 0x2C
// y trabajador 0x2D; núcleo ideal 2, puede usar cualquiera de los permitidos si está libre). Cada 2 s (o al avisarle
// frame_tick) vacía g_out_sink a la SD. Medidas para [LOGF]: duración de cada vaciado y bytes.
Thread g_logbg_thr;
std::atomic<bool> g_logbg_on{false}, g_logbg_quit{false};
std::mutex g_logbg_mx;
std::condition_variable g_logbg_cv;
bool g_logbg_kick = false;
std::atomic<uint64_t> st_bg_n{0}, st_bg_sum_ticks{0}, st_bg_max_ticks{0}, st_bg_bytes{0};
void logbg_main(void *) {
  u64 allowed = 0;
  svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  if (allowed & 4) svcSetThreadCoreMask(CUR_THREAD_HANDLE, 2, (u32)(allowed & 7));
  while (!g_logbg_quit.load(std::memory_order_relaxed)) {
    {
      std::unique_lock<std::mutex> lk(g_logbg_mx);
      g_logbg_cv.wait_for(lk, std::chrono::seconds(2), [] { return g_logbg_kick || g_logbg_quit.load(); });
      g_logbg_kick = false;
    }
    const uint64_t t0 = armGetSystemTick();
    const size_t n = g_out_sink.drain();
    if (n) {
      const uint64_t d = armGetSystemTick() - t0;
      st_bg_n.fetch_add(1, std::memory_order_relaxed);
      st_bg_sum_ticks.fetch_add(d, std::memory_order_relaxed);
      st_bg_bytes.fetch_add(n, std::memory_order_relaxed);
      if (d > st_bg_max_ticks.load(std::memory_order_relaxed)) st_bg_max_ticks.store(d, std::memory_order_relaxed);
    }
  }
}
void logbg_start() {
  // m6i: ENCENDIDO por defecto. Se lee aquí (setup_logging, dentro de early_init: ANTES de m6_defaults() de main.cpp),
  // con la misma regla que m6_defaults: un valor explícito manda; NWII_M_LOGBG_OFF / NWII_M_OFF / NWII_M6_OFF lo apagan.
  const char *e = std::getenv("NWII_M_LOGBG");
  const bool on = e ? (*e && e[0] != '0')
                    : !(std::getenv("NWII_M_LOGBG_OFF") || std::getenv("NWII_M_OFF") || std::getenv("NWII_M6_OFF"));
  if (!on) return;
  {
    std::lock_guard<std::mutex> lk(g_out_sink.m);
    g_out_sink.bg = true;  // antes de la primera escritura de stdout (setup_logging)
  }
  const Result rc = threadCreate(&g_logbg_thr, logbg_main, nullptr, nullptr, 64 * 1024, 0x3B, -2);
  if (R_SUCCEEDED(rc) && R_SUCCEEDED(threadStart(&g_logbg_thr))) {
    g_logbg_on = true;
  } else {
    std::lock_guard<std::mutex> lk(g_out_sink.m);
    g_out_sink.bg = false;  // sin hilo: modo de siempre
  }
}
void logbg_stop() {
  if (!g_logbg_on.exchange(false)) return;
  {
    std::lock_guard<std::mutex> lk(g_logbg_mx);
    g_logbg_quit = true;
  }
  g_logbg_cv.notify_one();
  threadWaitForExit(&g_logbg_thr);
  threadClose(&g_logbg_thr);
}

ssize_t out_write(struct _reent *, void *, const char *ptr, size_t len) {
  return g_out_sink.write(ptr, len);
}
ssize_t err_write(struct _reent *, void *, const char *ptr, size_t len) {
  return g_err_sink.write(ptr, len);
}

devoptab_t g_out_dotab = {};
devoptab_t g_err_dotab = {};

enum class LogMode { Quiet, File, Nxlink };
LogMode g_log_mode = LogMode::Quiet;
char g_stdout_buf[64 * 1024];

std::atomic<bool> g_cpu_done{false};

// ---------------------------------------------------------------- env.txt
std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return {};
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// KEY=VALUE per line, '#' comments. The runtime tests most NWII_* switches
// for presence only, so "0", "off", "false", "no" or an empty value removes
// the variable (NWII_REALTIME=0 really turns the default off). NWII_HOST_FPS
// is numeric and keeps its 0 (= uncapped); NWII_SHADER_PREWARM and
// NWII_SHADERCACHE test for the value "0" and keep it too.
void apply_env(const std::string &key, const std::string &val);
bool file_exists(const char *path);

void load_env_file() {
  FILE *f = std::fopen(g_env_file.c_str(), "r");
  if (!f) return;
  char line[512];
  while (std::fgets(line, sizeof line, f)) {
    std::string s = trim(line);
    if (s.empty() || s[0] == '#') continue;
    size_t eq = s.find('=');
    if (eq == std::string::npos) continue;
    apply_env(trim(s.substr(0, eq)), trim(s.substr(eq + 1)));
  }
  std::fclose(f);
}

void apply_env(const std::string &key, const std::string &val) {
    if (key.empty()) return;
    std::string lv = val;
    std::transform(lv.begin(), lv.end(), lv.begin(), ::tolower);
    // These read the value itself and treat "0" as "off": keep their 0.
    const bool keeps_zero = key == "NWII_HOST_FPS" || key == "NWII_SHADER_PREWARM" ||
                            key == "NWII_SHADERCACHE" || key == "NWII_NX_SIX_ROTATE" ||
                            key == "NWII_SHADER_SSO" || key == "NWII_SHADER_PREWARM_DRAW" ||
                            key == "NWII_AX_THREAD" || key == "NWII_NX_PIN_MAIN" ||
                            key == "NWII_FIFO_MAX_FRAMES" || key == "NWII_SHADER_PREWARM_BAR" ||
                            // R22 R7: on by default, "0" turns them off
                            key == "NWII_DLMEMO" || key == "NWII_PINDEX" || key == "NWII_RENDER_FREE" ||
                            key == "NWII_DLMEMO_LIST" || key == "NWII_WPROF" ||
                            // r31t: encendido por defecto, "0" lo apaga
                            key == "NWII_T_EFBVAR";
    const bool off = lv.empty() || lv == "off" || lv == "false" || lv == "no" ||
                     (lv == "0" && !keeps_zero);
    if (off)
      unsetenv(key.c_str());
    else
      setenv(key.c_str(), val.c_str(), 1);
}

bool parse_toml(const std::string &path, toml::table &tbl) {
  if (!file_exists(path.c_str())) return false;
  try {
    tbl = toml::parse_file(path);
    return true;
  } catch (const toml::parse_error &e) {
    std::fprintf(stderr, "[NX] %s: %s\n", path.c_str(), e.what());
    return false;
  }
}

// config.toml [switch]: the player-facing options. debug.toml (developers
// only; never shipped, never written by the NRO) holds the diagnostic ones
// plus raw NWII_* = value pairs.
void load_switch_options() {
  toml::table cfg, dbg;
  auto flag = [](toml::table &t, const char *key, const char *env, bool env_when) {
    if (auto v = t[key].value<bool>()) apply_env(env, *v == env_when ? "1" : "");
  };
  if (parse_toml(g_config, cfg) && cfg["switch"].as_table()) {
    toml::table &sw = *cfg["switch"].as_table();
    flag(sw, "motion", "NWII_NX_NO_MOTION", false);
    flag(sw, "tilt_invert", "NWII_NX_TILT_INVERT", true);
    flag(sw, "fps_overlay", "NWII_FPS_OVERLAY", true);  // r10: on-screen fps / frame time / thread load
  }
  if (!parse_toml(g_root + "/debug.toml", dbg)) return;
  flag(dbg, "fps_log", "NWII_LOG", true);
  flag(dbg, "cpu_boost", "NWII_NX_CPUBOOST", true);
  flag(dbg, "profile", "NWII_NX_PROFILE", true);  // sampling profiler (switch_profiler.cpp)
  flag(dbg, "fps_overlay", "NWII_FPS_OVERLAY", true);  // r10: on-screen fps overlay
  if (auto v = dbg["joycon_rotate"].value<bool>()) apply_env("NWII_NX_SIX_ROTATE", *v ? "1" : "0");
  if (auto v = dbg["shader_prewarm"].value<bool>()) apply_env("NWII_SHADER_PREWARM", *v ? "1" : "0");
  for (auto &&[k, v] : dbg) {
    std::string key(k.str());
    // Also the Mesa/nouveau knobs (NV50_PROG_OPTIMIZE, MESA_*, GALLIUM_*):
    // diagnostics only, debug.toml is never shipped.
    if (key.rfind("NWII_", 0) != 0 && key.rfind("NV50_", 0) != 0 &&
        key.rfind("MESA_", 0) != 0 && key.rfind("GALLIUM_", 0) != 0)
      continue;
    if (auto str = v.value<std::string>()) apply_env(key, *str);
    else if (auto num = v.value<int64_t>()) apply_env(key, std::to_string(*num));
    else if (auto b = v.value<bool>()) apply_env(key, *b ? "1" : "");
  }
}

bool env_on(const char *name) {
  const char *v = std::getenv(name);
  return v && *v;
}

// ---------------------------------------------------------------- sdmc:
// libnx's SD card devoptab differs from POSIX in two ways the emulated NAND
// (hle/devices/fs_device.cpp, std::filesystem) depends on:
//  - no lstat: std::filesystem::remove_all/symlink_status fail with ENOSYS;
//  - rename() fails when the destination exists (Horizon's RenameFile does not
//    replace), where POSIX replaces it atomically.
// The game saves through "write /tmp/sys/wiimj2d.sav, rename over the real
// one", so without this the first save fails and the game shows its
// "could not save" dialog. Patched once at start on the mounted device.
devoptab_t g_sdmc_dotab;
const devoptab_t *g_sdmc_orig = nullptr;

int sdmc_lstat(struct _reent *r, const char *file, struct stat *st) {
  return g_sdmc_orig->stat_r(r, file, st);  // no symlinks on FAT/exFAT
}

int sdmc_rename(struct _reent *r, const char *old_name, const char *new_name) {
  struct stat st_old, st_new;
  if (std::strcmp(old_name, new_name) != 0 &&
      g_sdmc_orig->stat_r(r, new_name, &st_new) == 0 &&
      g_sdmc_orig->stat_r(r, old_name, &st_old) == 0) {
    const bool old_dir = S_ISDIR(st_old.st_mode), new_dir = S_ISDIR(st_new.st_mode);
    if (!old_dir && !new_dir)
      g_sdmc_orig->unlink_r(r, new_name);
    else if (old_dir && new_dir)
      g_sdmc_orig->rmdir_r(r, new_name);  // POSIX: only an empty directory
  }
  return g_sdmc_orig->rename_r(r, old_name, new_name);
}

void patch_sdmc_devoptab() {
  const int idx = FindDevice("sdmc:");
  if (idx < 0 || !devoptab_list[idx] || g_sdmc_orig) return;
  g_sdmc_orig = devoptab_list[idx];
  g_sdmc_dotab = *g_sdmc_orig;
  if (!g_sdmc_dotab.lstat_r) g_sdmc_dotab.lstat_r = sdmc_lstat;
  if (g_sdmc_orig->rename_r && g_sdmc_orig->stat_r && g_sdmc_orig->unlink_r)
    g_sdmc_dotab.rename_r = sdmc_rename;
  devoptab_list[idx] = &g_sdmc_dotab;
}

bool file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

void make_dirs_for(const std::string &file_prefix) {
  // mkdir -p for the directory part of a path like sdmc:/switch/NSMBWnx/shots/t
  size_t root = file_prefix.find(":/");
  size_t pos = root == std::string::npos ? 0 : root + 2;
  while ((pos = file_prefix.find('/', pos)) != std::string::npos) {
    mkdir(file_prefix.substr(0, pos).c_str(), 0777);
    ++pos;
  }
}

// ---------------------------------------------------------------- messages
// Text screen (libnx console) shown before SDL owns the display. Returns true
// to continue (A) and false to quit (+, or the system asked us to exit).
bool message_screen(const std::string &text, bool allow_continue) {
  const devoptab_t *saved_out = devoptab_list[STD_OUT];
  consoleInit(nullptr);
  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);

  std::printf("\n  NSMBW Recomp\n  ------------\n\n%s\n\n", text.c_str());
  if (allow_continue)
    std::printf("  A: continuar de todos modos / continue anyway\n");
  std::printf("  +: salir / exit\n");
  std::fflush(stdout);

  bool cont = false;
  while (appletMainLoop()) {
    padUpdate(&pad);
    const u64 down = padGetButtonsDown(&pad);
    if (down & HidNpadButton_Plus) break;
    if (allow_continue && (down & HidNpadButton_A)) {
      cont = true;
      break;
    }
    consoleUpdate(nullptr);
  }
  consoleExit(nullptr);
  devoptab_list[STD_OUT] = saved_out;
  return cont;
}

bool check_game_files() {
  const std::string dol = g_game_dir + "/sys/main.dol";
  if (!file_exists(dol.c_str()) ||
      !file_exists((g_game_dir + "/files").c_str())) {
    message_screen(
        "  No se encontraron los archivos del juego.\n"
        "  Copia las carpetas sys/ y files/ de tu disco\n"
        "  New Super Mario Bros. Wii (USA, SMNE01) a:\n"
        "      " + g_game_dir + "/sys\n"
        "      " + g_game_dir + "/files\n"
        "\n"
        "  Game files not found. Copy sys/ and files/ of your\n"
        "  NSMBW (USA, SMNE01) disc to " + g_game_dir + "/.",
        false);
    return false;
  }
#ifdef NWII_NX_EXPECTED_DOL_SHA256
  // The native code was generated from one exact main.dol; another revision
  // or region would crash at random points.
  if (FILE *f = std::fopen(dol.c_str(), "rb")) {
    std::vector<uint8_t> data;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 0) {
      data.resize((size_t)size);
      data.resize(std::fread(data.data(), 1, data.size(), f));
    }
    std::fclose(f);
    uint8_t digest[SHA256_HASH_SIZE];
    sha256CalculateHash(digest, data.data(), data.size());
    char hex[SHA256_HASH_SIZE * 2 + 1];
    for (int i = 0; i < SHA256_HASH_SIZE; ++i)
      std::snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (std::strcmp(hex, NWII_NX_EXPECTED_DOL_SHA256) != 0) {
      std::cerr << "[NX] main.dol sha256 " << hex << " != expected "
                << NWII_NX_EXPECTED_DOL_SHA256 << std::endl;
      return message_screen(
          "  Tu main.dol no es el de NSMBW USA (SMNE01, revision 1)\n"
          "  con el que se genero este port: el juego fallaria.\n"
          "  Revisa que sea el disco americano (SMNE01).\n\n"
          "  main.dol does not match NSMBW USA (SMNE01 rev 1),\n"
          "  the version this port was built from.",
          true);
    }
  }
#endif
  return true;
}

uint64_t mem_info(InfoType what) {
  u64 v = 0;
  svcGetInfo(&v, what, CUR_PROCESS_HANDLE, 0);
  return v;
}

// Memory as the game sees it: the loaded image (code + data + bss) plus what
// malloc took from the heap (svcGetInfo's "used" counts the whole heap libnx
// reserves at start, so it says nothing about the real need).
uint64_t g_heap_peak = 0;
void log_memory(const char *tag) {
  const struct mallinfo mi = mallinfo();
  // __start__ is an absolute 0 in switch.ld; the image starts at the base of
  // the code mapping that holds this function.
  MemoryInfo region = {};
  u32 page_info = 0;
  svcQueryMemory(&region, &page_info, (u64)(uintptr_t)&log_memory);
  const uint64_t image = (uint64_t)((uintptr_t)__end__ - region.addr);
  const uint64_t heap = (uint64_t)mi.arena;
  if (heap > g_heap_peak) g_heap_peak = heap;
  std::printf("[NX] memory %s: image %llu MB + heap %llu MB (in use %llu MB, peak %llu MB)"
              " = %llu MB; process total %llu MB\n",
              tag, (unsigned long long)(image >> 20), (unsigned long long)(heap >> 20),
              (unsigned long long)((uint64_t)mi.uordblks >> 20),
              (unsigned long long)(g_heap_peak >> 20),
              (unsigned long long)((image + g_heap_peak) >> 20),
              (unsigned long long)(mem_info(InfoType_TotalMemorySize) >> 20));
  // R8 memory: how much of the heap is free and where. newlib never gives the
  // heap back, so "free" is reusable space: the top chunk (keepcost) can serve
  // any request, the rest are holes between live blocks (fragmentation).
  const uint64_t fr = (uint64_t)mi.fordblks, top = (uint64_t)mi.keepcost;
  std::printf("[NX] heap detail: arena %.1f MB, in use %.1f MB, free %.1f MB (top %.1f MB,"
              " holes %.1f MB in %d chunks)\n",
              mi.arena / 1048576.0, mi.uordblks / 1048576.0, fr / 1048576.0, top / 1048576.0,
              (fr > top ? fr - top : 0) / 1048576.0, (int)mi.ordblks);
  if (std::strcmp(tag, "at start") != 0) {
    char buf[512];
    const size_t acc = GX_MemReport(buf, sizeof buf);
    const uint64_t used = (uint64_t)mi.uordblks;
    std::printf("[NX] mem: %s; rest of the heap in use %.1f MB (GL driver, game RAM, code)\n",
                buf, (used > acc ? used - acc : 0) / 1048576.0);
  }
}

bool is_application() {
  const AppletType t = appletGetAppletType();
  return t == AppletType_Application || t == AppletType_SystemApplication;
}

// What the game needs: the loaded image (110 MB of recompiled code) plus a
// heap of MEM1/MEM2 (88 MB), the dispatch table (24 MB), the CPU thread stack
// (32 MB), textures and the GL driver. Measured in citron (2026-10-26): 431 MB
// at the title screen, before any level. Applet mode (~400-440 MB) is short.
constexpr uint64_t kMinMemory = 512ull << 20;

bool check_memory() {
  const uint64_t total = mem_info(InfoType_TotalMemorySize);
  if (total >= kMinMemory || is_application()) return true;
  char text[512];
  std::snprintf(
      text, sizeof text,
      "  Modo applet (abierto desde el Album): solo hay %llu MB de memoria\n"
      "  y el juego necesita unos %llu MB. Abrelo manteniendo R al iniciar\n"
      "  cualquier juego (\"title takeover\") para tener toda la memoria.\n\n"
      "  Applet mode: only %llu MB of memory. Launch it by holding R while\n"
      "  starting any game (title takeover) to get the full memory.",
      (unsigned long long)(total >> 20), (unsigned long long)(kMinMemory >> 20),
      (unsigned long long)(total >> 20));
  return message_screen(text, true);
}

void setup_logging() {
  // NWII_LOG: unset/0 = quiet (errors only, to nwii_err.log), 1 = full log
  // to nwii.log in the NRO folder, nxlink = stdout/stderr over the network to
  // `nxlink -s` on the PC.
  const char *mode = std::getenv("NWII_LOG");
  if (mode && std::strcmp(mode, "nxlink") == 0) {
    if (R_SUCCEEDED(socketInitializeDefault()) && nxlinkStdio() >= 0) {
      g_log_mode = LogMode::Nxlink;
      return;
    }
    mode = "1";  // no nxlink host: fall back to the log file
  }

  g_err_dotab.name = "nwiierr";
  g_err_dotab.write_r = err_write;
  remove(g_err_sink.path.c_str());  // no stale errors from an earlier run
  devoptab_list[STD_ERR] = &g_err_dotab;
  setvbuf(stderr, nullptr, _IONBF, 0);

  if (mode && *mode) {
    g_log_mode = LogMode::File;
    g_out_dotab.name = "nwiiout";
    g_out_dotab.write_r = out_write;
    devoptab_list[STD_OUT] = &g_out_dotab;
    setvbuf(stdout, g_stdout_buf, _IOFBF, sizeof g_stdout_buf);
    logbg_start();  // r41m NWII_M_LOGBG
  } else {
    // Quiet: stdout stays on libnx's null device, and a stream in a failed
    // state skips all std::cout formatting (~250 call sites, some per
    // interrupt). printf output still goes to the null device.
    g_log_mode = LogMode::Quiet;
    setvbuf(stdout, g_stdout_buf, _IOFBF, sizeof g_stdout_buf);
    std::cout.setstate(std::ios_base::badbit);
  }
}

void log_header() {
  const char *mode_name = g_log_mode == LogMode::File     ? "file"
                          : g_log_mode == LogMode::Nxlink ? "nxlink"
                                                          : "quiet";
  std::printf(
      "[NX] NSMBW Recomp (Switch) built " __DATE__ " " __TIME__
      "\n[NX] applet type %d (%s), operation mode %s, log %s\n"
      "[NX] NWII_REALTIME=%s\n",
      (int)appletGetAppletType(), is_application() ? "application" : "applet",
      appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld",
      mode_name, std::getenv("NWII_REALTIME") ? std::getenv("NWII_REALTIME") : "(off)");
  log_memory("at start");
  std::fflush(stdout);
}

// hbmenu passes the NRO's path as argv[0] ("sdmc:/switch/NSMBWnx/NSMBWnx.nro").
void choose_root(int argc, char **argv) {
  auto has_game = [](const std::string &root) {
    return file_exists((root + "/data/sys/main.dol").c_str()) ||
           file_exists((root + "/SMNE01/sys/main.dol").c_str());
  };
  // The NRO's folder when hbmenu passed its path (emulators may not), then
  // the usual install folder, then the old one: the first holding the game
  // wins, else the NRO's folder (or the usual one) so its message shows there.
  std::vector<std::string> roots;
  std::string nro = argc >= 1 && argv[0] ? argv[0] : "";
  size_t slash = nro.rfind('/');
  if (nro.rfind("sdmc:/", 0) == 0 && slash != std::string::npos && slash > 5)
    roots.push_back(nro.substr(0, slash));
  roots.push_back("sdmc:/switch/NSMBWnx");
  roots.push_back(kLegacyRoot);
  g_root = roots.front();
  for (const std::string &r : roots)
    if (has_game(r)) {
      g_root = r;
      break;
    }
  auto pick = [](const std::string &now, const std::string &old) {
    return !file_exists(now.c_str()) && file_exists(old.c_str()) ? old : now;
  };
  g_game_dir = pick(g_root + "/data", g_root + "/SMNE01");
  g_config = pick(g_root + "/config.toml", g_root + "/nsmbw.toml");
  g_env_file = g_root + "/env.txt";  // old layout; config.toml [switch] replaces it
  g_out_sink.path = g_root + "/logs/nwii.log";
  g_err_sink.path = g_root + "/logs/nwii_err.log";
}

}  // namespace

void nx_fast_new_link();  // switch_alloc.cpp

bool early_init(int *argc, char ***argv) {
  choose_root(*argc, *argv);
  note_thread(kThreadMain);
  clkrstInitialize();  // r10b: the fps_log line reports the real CPU/GPU clocks
  nx_fast_new_link();  // r10b: pulls in the fast operator new/delete (switch_alloc.cpp)
  // An NRO gets no arguments (nxlink may pass some: then they win).
  static const char *nx_argv[] = {"nsmbw", g_game_dir.c_str(), g_config.c_str(), nullptr};
  if (*argc < 2) {
    *argc = 3;
    *argv = (char **)nx_argv;
  }

  patch_sdmc_devoptab();  // POSIX lstat/rename for the emulated NAND
  mkdir(g_root.c_str(), 0777);
  mkdir((g_root + "/cache").c_str(), 0777);

  // Production defaults; config.toml [switch] / env.txt can override them.
  setenv("NWII_REALTIME", "1", 1);  // wall-clock timebase, one 60 Hz VI source
  // R4's streaming vertex/index ring (glBufferSubData into 24+8 MB buffers)
  // is not proven on Switch Mesa (one heap-corruption crash in citron with
  // it, none in 11 runs with the per-batch glBufferData path): keep the old
  // path by default. env.txt NWII_NORING=0 enables the ring.
  setenv("NWII_NORING", "1", 1);
  setenv("NWII_SHADERCACHE", (g_root + "/cache/shadercache.bin").c_str(), 1);

  if (!file_exists(g_config.c_str())) {
    if (FILE *f = std::fopen(g_config.c_str(), "w")) {
      std::fputs(kDefaultToml, f);
      std::fclose(f);
    }
  }
  load_switch_options();
  load_env_file();
  if (const char *dump = std::getenv("NWII_XFBDUMP")) make_dirs_for(dump);
  if (const char *shot = std::getenv("NWII_SCREENSHOT")) make_dirs_for(shot);

  setup_logging();
  log_header();
  // R8 thread layout (homebrew gets cores 0-2): main = render + GX + audio
  // feed on core 0, recompiled CPU on core 1 (cpu_thread_start), AX mixer on
  // core 2 (ax_async.cpp). Pin the main thread so it never lands on 1 or 2
  // (debug.toml NWII_NX_PIN_MAIN = "0" leaves it on the default core).
  if (!std::getenv("NWII_NX_PIN_MAIN") || std::getenv("NWII_NX_PIN_MAIN")[0] != '0') {
    u64 allowed = 0;
    svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    const Result rc = (allowed & 1) ? svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 1) : 0;
    std::printf("[NX] main thread on core 0: %s (allowed mask 0x%llx)\n",
                R_SUCCEEDED(rc) ? "ok" : "failed", (unsigned long long)allowed);
  }

  profiler_start();  // debug.toml profile = true

  if (!check_game_files()) return false;
  if (!check_memory()) return false;

  // Opt-in: CPU at 1785 MHz (the GPU drops to 76.8 MHz while boosted).
  if (env_on("NWII_NX_CPUBOOST")) {
    Result rc = appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
    std::printf("[NX] CPU boost (FastLoad): %s\n", R_SUCCEEDED(rc) ? "on" : "failed");
  }
  return true;
}

// R8: per-thread CPU time for fps_log ("threads main x% cpu y% ax z%"), from
// the kernel's per-thread tick count (19.2 MHz).
static std::atomic<Handle> g_thread_handles[kThreadSlots];
static const char *const kThreadNames[kThreadSlots] = {"main", "cpu", "ax", "aud", "gxw", "snd"};

void note_thread(int slot) {
  if (slot >= 0 && slot < kThreadSlots)
    g_thread_handles[slot].store(threadGetCurHandle());
}

// r10: share of a core used by the render (main) and game CPU threads over the
// last ~0.5 s, for the on-screen overlay (renderer_gl.cpp). -1 = unknown.
void overlay_thread_load(float *render, float *game) {
  static uint64_t last_t = 0, last_main = 0, last_cpu = 0;
  static float r = -1.f, g = -1.f;
  static bool unsupported = false;
  const uint64_t now = armGetSystemTick();
  if (!unsupported && now - last_t >= 19200000 / 2) {
    const Handle hm = g_thread_handles[kThreadMain].load(), hc = g_thread_handles[kThreadCpu].load();
    u64 tm = 0, tc = 0;
    if (hm && hc) {
      if (R_FAILED(svcGetInfo(&tm, InfoType_ThreadTickCount, hm, (u64)-1)) ||
          R_FAILED(svcGetInfo(&tc, InfoType_ThreadTickCount, hc, (u64)-1))) {
        unsupported = true;
      } else {
        if (last_t) {
          const double dt = (double)(now - last_t);
          r = (float)((double)(tm - last_main) / dt);
          g = (float)((double)(tc - last_cpu) / dt);
        }
        last_main = tm;
        last_cpu = tc;
        last_t = now;
      }
    }
  }
  *render = r;
  *game = g;
}

unsigned thread_handle(int slot) {
  return slot >= 0 && slot < kThreadSlots ? g_thread_handles[slot].load() : 0;
}

const char *root_dir() { return g_root.c_str(); }

static void thread_report(char *buf, size_t n, double dt) {
  static uint64_t last[kThreadSlots] = {};
  static bool unsupported = false;  // citron: no per-thread tick count
  size_t used = 0;
  buf[0] = 0;
  for (int i = 0; i < kThreadSlots && !unsupported; ++i) {
    const Handle h = g_thread_handles[i].load();
    u64 ticks = 0;
    if (!h) continue;
    if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, (u64)-1)) &&
        R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCountDeprecated, h, (u64)-1))) {
      unsupported = true;
      break;
    }
    const double pct = last[i] ? 100.0 * (double)(ticks - last[i]) / 19200000.0 / dt : 0.0;
    last[i] = ticks;
    used += std::snprintf(buf + used, n - used, "%s %s %.0f%%", used ? "," : ", threads",
                          kThreadNames[i], pct);
    if (used >= n) break;
  }
}

void cpu_thread_start() {
  note_thread(kThreadCpu);
  // libnx starts every thread on the process' default core, where the main
  // (render + audio feed) thread already runs; the recompiled CPU is the
  // bottleneck and gets its own core.
  u64 allowed = 0;
  svcGetInfo(&allowed, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
  for (int core : {1, 2}) {
    if (!(allowed & (1ull << core))) continue;
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    if (R_SUCCEEDED(rc)) {
      std::printf("[NX] CPU thread on core %d (allowed mask 0x%llx)\n", core,
                  (unsigned long long)allowed);
      return;
    }
  }
  std::printf("[NX] CPU thread left on the default core (allowed mask 0x%llx)\n",
              (unsigned long long)allowed);
}

void cpu_thread_finished() { g_cpu_done.store(true); }
bool cpu_thread_done() { return g_cpu_done.load(); }

void frame_tick() {
  using clk = std::chrono::steady_clock;
  static clk::time_point last_flush = clk::now();
  static clk::time_point last_stat = clk::now();
  static uint64_t last_frames = 0;
  static uint64_t host_frames = 0;
  ++host_frames;
  const clk::time_point now = clk::now();

  if (g_log_mode == LogMode::File &&
      now - last_stat >= std::chrono::seconds(5)) {
    const double dt = std::chrono::duration<double>(now - last_stat).count();
    const uint64_t frames = g_stat_frames;
    char gov[384], thr[128], aud[320];
    nwii::runtime::governor_report(gov, sizeof gov, dt);
    thread_report(thr, sizeof thr, dt);
    audio_report(aud, sizeof aud, dt);
    // r10b: the real clocks (overclock or not) next to every measurement.
    u32 cpu_hz = 0, gpu_hz = 0;
    {
      ClkrstSession s;
      if (R_SUCCEEDED(clkrstOpenSession(&s, PcvModuleId_CpuBus, 3))) { clkrstGetClockRate(&s, &cpu_hz); clkrstCloseSession(&s); }
      if (R_SUCCEEDED(clkrstOpenSession(&s, PcvModuleId_GPU, 3))) { clkrstGetClockRate(&s, &gpu_hz); clkrstCloseSession(&s); }
    }
    std::printf("[NX] game %.1f fps, host loop %.1f/s, cpu %u MHz, gpu %u MHz%s%s%s\n", (frames - last_frames) / dt,
                host_frames / dt, cpu_hz / 1000000, gpu_hz / 1000000, gov, thr, aud);
    if (nwii::runtime::s2::g_enabled) {  // r34s2
      char s2l[2048];  // r38c: + linea [C38]; r41m m6h: + [A43]
      nwii::runtime::s2::stats_line(s2l, sizeof s2l, dt);
      std::printf("%s\n", s2l);
    }
    log_memory("now");
    last_frames = frames;
    host_frames = 0;
    last_stat = now;
  }
  // r41m (frente M): medida del vaciado periódico ([LOGF], cada 5 s). Sin NWII_M_LOGBG: fflush(stdout) +
  // g_out_sink.flush() (fwrite hasta 256 KB + fsync a la SD) en ESTE hilo (render) con el mutex del sumidero tomado.
  // Con NWII_M_LOGBG: aquí solo fflush(stdout) (copia a memoria) y aviso al hilo de fondo.
  static uint64_t lf_n = 0, lf_sum = 0, lf_max = 0;
  static clk::time_point last_lf = now;
  if (now - last_flush >= std::chrono::seconds(2)) {
    last_flush = now;
    if (g_log_mode == LogMode::File) {
      const uint64_t t0 = armGetSystemTick();
      std::fflush(stdout);
      if (g_logbg_on.load(std::memory_order_relaxed)) {
        { std::lock_guard<std::mutex> lk(g_logbg_mx); g_logbg_kick = true; }
        g_logbg_cv.notify_one();
      } else {
        g_out_sink.flush();
      }
      const uint64_t d = armGetSystemTick() - t0;
      ++lf_n; lf_sum += d; lf_max = std::max(lf_max, d);
    }
  }
  if (g_log_mode == LogMode::File && now - last_lf >= std::chrono::seconds(5)) {
    last_lf = now;
    auto ms = [](uint64_t t) { return armTicksToNs(t) / 1e6; };
    const uint64_t wn = g_out_sink.st_w_n.exchange(0), wmax = g_out_sink.st_w_max_ticks.exchange(0);
    const uint64_t bn = st_bg_n.exchange(0), bsum = st_bg_sum_ticks.exchange(0), bmax = st_bg_max_ticks.exchange(0),
                   bb = st_bg_bytes.exchange(0);
    size_t drop;
    { std::lock_guard<std::mutex> lk(g_out_sink.m); drop = g_out_sink.pend_drop; }
    std::printf("[LOGF] %s | vaciado en render: n %llu media %.2f ms max %.2f ms | write() n %llu max %.2f ms | "
                "fondo: n %llu media %.2f ms max %.2f ms %.0f KB | tirados %zu B\n",
                g_logbg_on.load() ? "fondo" : "sincrono", (unsigned long long)lf_n, lf_n ? ms(lf_sum) / lf_n : 0.0,
                ms(lf_max), (unsigned long long)wn, ms(wmax), (unsigned long long)bn, bn ? ms(bsum) / bn : 0.0, ms(bmax),
                bb / 1024.0, drop);
    lf_n = lf_sum = lf_max = 0;
  }
}

// r38c: el vigilante de cuelgues (s2.cpp) vuelca su informe al registro normal y lo lleva a la SD ya.
void c38_sink_flush() {
  if (g_log_mode != LogMode::File) return;
  std::fflush(stdout);
  g_out_sink.flush();
}

void shutdown() {
  rumble(false);
  std::fflush(stdout);
  std::fflush(stderr);
  logbg_stop();  // r41m NWII_M_LOGBG (close() vacía lo pendiente)
  g_out_sink.close();
  g_err_sink.close();
}

}  // namespace nwii::runtime::nx
#endif  // __SWITCH__
