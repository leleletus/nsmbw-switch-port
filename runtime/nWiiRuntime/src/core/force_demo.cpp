// force_demo.cpp (r21s, frente S): escenas repetibles para medir.
//
// Solo depuración, apagado por defecto (sin variables no hace nada: ios.cpp solo mira un bool).
//   NWII_FORCE_DEMO        = lista ordenada de demos del título, separadas por comas: nombre corto,
//                            índice (0-14), mundo-nivel ("2-1") o "castillo" (la cinemática del
//                            título). Una sola = siempre esa. Al acabar la lista vuelve a empezar.
//   NWII_FORCE_DEMO_REPEAT = N: cada elemento N veces seguidas (1 por defecto).
//   NWII_FORCE_DEMO_LOOP   = 1: sin los 15 s de la pantalla de título entre una demo y la siguiente.
//   NWII_DEMO_LOG          = 1: solo las líneas [DEMO] start/end (sin forzar nada); 2: además el
//                            estado crudo (depuración de esto mismo).
//   NWII_FORCE_LEVEL       = "1-1", "3-T", "6-C", "5-G": arranca directo en ese nivel (modo normal,
//                            jugador 1 = Mario) en vez de la pantalla de título. Lleva NWII_NO_SAVE.
//   NWII_NO_SAVE           = 1: (en hle/devices/fs_device.cpp) la NAND se copia a <nand>_prueba al
//                            arrancar y el juego escribe ahí; la partida real no se toca.
// Líneas para cortar estadísticas por escena: "[DEMO] start <id> at <s>" / "[DEMO] end <id> at <s>
// (<duración> s)", segundos desde el arranque del runtime.
//
// Cómo elige el juego (decomp: dScRestartCrsin_c::startTitle, d_s_restart_crsin_static.cpp):
//   demo = c_stage[m_titleRandomTable[m_titleCount]], tabla de 15 barajada una vez al arrancar
//   (dScStage_c::setTitleReplayRandomTable desde dScBoot_c::create). En la pantalla de título
//   (dScStage, modo TITLE) tras 900 frames: si (contador % 4) != 0 -> demo, si no -> MOVIE.
// Lo único que se cambia es el resultado de esa elección: la entrada de la tabla que se va a
// leer (y, para la cinemática, el contador que decide demo/MOVIE). El RNG del juego no se toca.
// Se ejecuta en el hilo de la CPU, en el evento de VI (60/s de tiempo del juego), ver ios.cpp.
#include "runtime/cpu_context.h"
#include "runtime/nwii_env.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>

namespace nwii::runtime {

bool g_force_demo_on = false;
extern std::atomic<int> g_tprof_scene;  // r31t tprof.cpp

namespace {

// Direcciones (USA, SMNE01). r13 = 0x8042F680.
constexpr uint32_t kScene = 0x80428732;       // u16: perfil de la escena actual (5 STAGE, 8 MOVIE...)
constexpr uint32_t kTable = 0x803741BC;       // u8[15] dScStage_c::m_titleRandomTable
constexpr uint32_t kTitleCount = 0x8042A1F8;  // int dScStage_c::m_titleCount (r13-21640)
constexpr uint32_t kMovieCount = 0x8042A1F4;  // int: % 4 == 0 -> MOVIE en vez de demo
constexpr uint32_t kExeFrame = 0x8042A200;    // u32 frames de la escena STAGE
constexpr uint32_t kGameMode = 0x8042A204;    // dScStage_c::m_gameMode (2 TITLE, 3 TITLE_REPLAY)
constexpr uint32_t kSgi = 0x80373D60;         // dScRestartCrsin_c::m_startGameInfo
constexpr uint32_t kStageAddr = 0x802F49D0;   // c_stage[15][2] (solo informativo)
constexpr int kMovie = 15;                    // "castillo": la cinemática del título (escena MOVIE)
constexpr int kSceneStage = 5, kSceneMovie = 8;

struct Demo { const char *id; uint8_t world, stage; const char *what; };
// Índice = valor de m_titleRandomTable. world/stage en base 0 (como c_stage).
const Demo kDemos[16] = {
    {"agua_pinguino", 0, 3, "1-4"},        // submarina, traje de pingüino (bolas de hielo)
    {"pradera", 0, 5, "1-6"},              // colinas verdes
    {"desierto4j", 1, 0, "2-1"},           // desierto con pirámide y géiser de arena
    {"desierto_helices", 1, 3, "2-4"},     // desierto con bloques y trajes hélice
    {"nieve", 2, 0, "3-1"},                // bosque nevado
    {"nubes_plataformas", 2, 4, "3-5"},    // cielo de nubes grises, setas rosas que giran
    {"torre3", 2, 21, "3-T"},              // torre del mundo 3 (piedra)
    {"playa_hielo", 3, 0, "4-1"},          // playa medio sumergida, enemigos en bloques de hielo
    {"playa_yoshi", 3, 4, "4-5"},          // playa con Yoshi, Lakitu y setas rosas
    {"jungla", 4, 3, "5-4"},               // jungla con pantano morado
    {"cielo_hoja", 4, 4, "5-5"},           // cielo con hoja voladora
    {"casa_oscura", 4, 20, "5-G"},         // casa fantasma del mundo 5
    {"castillo6", 5, 23, "6-C"},           // castillo del mundo 6 (lava)
    {"cielo_paratroopas", 6, 5, "7-6"},    // cielo con Para-Beetles rojos (caparazones voladores)
    {"lava", 7, 6, "8-7"},                 // cueva de lava
    {"castillo", 0, 0, "MOVIE"},           // cinemática del título (castillo de Peach, la tarta)
};

std::vector<int> g_list;  // lista expandida (REPEAT)
size_t g_pos = 0;
bool g_force = false, g_loop = false;
int g_log = 0;
bool g_table_ready = false;
int g_active = -1;  // lo que se está viendo ahora (-1 nada)
double g_active_t = 0;
uint32_t g_ticks = 0;
int g_lvl_w = -1, g_lvl_s = -1;  // NWII_FORCE_LEVEL (base 0)
bool g_lvl_done = false;

double secs() {
  using clk = std::chrono::steady_clock;
  static const clk::time_point t0 = clk::now();
  return std::chrono::duration<double>(clk::now() - t0).count();
}

const char *name_of(int i) { return (i >= 0 && i <= kMovie) ? kDemos[i].id : "?"; }

int lookup_stage(uint8_t w, uint8_t s) {
  for (int i = 0; i < kMovie; ++i)
    if (kDemos[i].world == w && kDemos[i].stage == s) return i;
  return -1;
}

int parse_one(std::string t) {
  while (!t.empty() && (t.front() == ' ' || t.front() == '"')) t.erase(t.begin());
  while (!t.empty() && (t.back() == ' ' || t.back() == '"')) t.pop_back();
  if (t.empty()) return -2;
  for (int i = 0; i <= kMovie; ++i)
    if (t == kDemos[i].id) return i;
  if (t == "movie" || t == "pelicula" || t == "cinematica") return kMovie;
  char *e = nullptr;
  long v = std::strtol(t.c_str(), &e, 10);
  if (e && *e == 0 && v >= 0 && v <= kMovie) return (int)v;
  for (int i = 0; i < kMovie; ++i)  // "2-1", "3-T", "5-G"...
    if (strcasecmp(kDemos[i].what, t.c_str()) == 0) return i;
  return -1;
}

}  // namespace

void force_demo_init() {
  const char *fd = std::getenv("NWII_FORCE_DEMO");
  const char *lg = std::getenv("NWII_DEMO_LOG");
  g_log = lg ? std::atoi(lg) : 0;
  if (fd && *fd) {
    int rep = 1;
    if (const char *r = std::getenv("NWII_FORCE_DEMO_REPEAT")) rep = std::max(1, std::atoi(r));
    std::string s = fd, tok;
    std::vector<int> base;
    for (size_t i = 0; i <= s.size(); ++i) {
      if (i == s.size() || s[i] == ',') {
        int v = parse_one(tok);
        if (v >= 0) base.push_back(v);
        else if (v == -1) std::printf("[DEMO] NWII_FORCE_DEMO: '%s' no existe (se ignora)\n", tok.c_str());
        tok.clear();
      } else {
        tok += s[i];
      }
    }
    for (int v : base)
      for (int k = 0; k < rep; ++k) g_list.push_back(v);
    g_force = !g_list.empty();
    const char *lp = std::getenv("NWII_FORCE_DEMO_LOOP");
    g_loop = g_force && lp && lp[0] == '1';
  }
  if (const char *lv = std::getenv("NWII_FORCE_LEVEL")) {
    // "1-1", "8-7", "3-T" (torre), "3-C" (castillo), "5-G" (casa fantasma)
    int w = 0, n = 0; char k = 0;
    if (std::sscanf(lv, "%d-%d", &w, &n) == 2 && w >= 1 && w <= 9 && n >= 1 && n <= 19) {
      g_lvl_w = w - 1; g_lvl_s = n - 1;
    } else if (std::sscanf(lv, "%d-%c", &w, &k) == 2 && w >= 1 && w <= 9) {
      k = (char)(k | 0x20);
      const int st = k == 't' ? 21 : k == 'c' ? 23 : k == 'g' ? 20 : -1;
      if (st >= 0) { g_lvl_w = w - 1; g_lvl_s = st; }
    }
    if (g_lvl_w < 0) std::printf("[DEMO] NWII_FORCE_LEVEL='%s' no se entiende (formato 1-1, 3-T, 3-C, 5-G)\n", lv);
  }
  if ((g_force || g_lvl_w >= 0) && g_log == 0) g_log = 1;
  g_force_demo_on = g_force || g_log > 0 || g_lvl_w >= 0;
  if (!g_force_demo_on) return;
  secs();
  std::string l;
  for (int v : g_list) { if (!l.empty()) l += ","; l += name_of(v); }
  if (g_lvl_w >= 0)
    std::printf("[DEMO] NWII_FORCE_LEVEL: arranque directo al nivel %d-%d (mundo/nivel base 0: %d/%d); "
                "NAND en copia de prueba (no se escribe la partida)\n", g_lvl_w + 1, g_lvl_s + 1, g_lvl_w, g_lvl_s);
  std::printf("[DEMO] forzar: %s | lista (%zu): %s | sin título entre demos: %s\n",
              g_force ? "sí" : "no (solo registro)", g_list.size(), l.empty() ? "-" : l.c_str(),
              g_loop ? "sí" : "no");
}

void force_demo_tick(CPUContext &c) {
  ++g_ticks;
  MMU &m = c.mmu;
  const int scene = (int)m.read16(kScene);
  const uint32_t gmode = m.read32(kGameMode);
  const uint32_t exef = m.read32(kExeFrame);

  if (g_log >= 2) {
    static int ls = -9; static uint32_t lgm = 99, lmc = 99, ltc = 99, lsm = 99; static uint8_t lw = 99, ll = 99;
    const uint32_t mc = m.read32(kMovieCount), tc = m.read32(kTitleCount), sm = m.read32(kSgi + 8);
    const uint8_t w = m.read8(kSgi + 12), lv = m.read8(kSgi + 13);
    if (scene != ls || gmode != lgm || mc != lmc || tc != ltc || sm != lsm || w != lw || lv != ll) {
      char tb[64]; int n = 0;
      for (int i = 0; i < 15; ++i) n += std::snprintf(tb + n, sizeof tb - n, "%x", m.read8(kTable + i));
      std::printf("[DEMO] raw t=%.2f tick=%u scene=%d gmode=%u exef=%u moviecnt=%d titlecnt=%d table=%s "
                  "sgi mode=%u w=%u l=%u replay=%u\n", secs(), g_ticks, scene, gmode, exef, (int)mc,
                  (int)tc, tb, sm, w, lv, m.read8(kSgi + 7));
      ls = scene; lgm = gmode; lmc = mc; ltc = tc; lsm = sm; lw = w; ll = lv;
    }
  }

  // 1) Qué se ve ahora: demo (STAGE en modo TITLE_REPLAY) o la cinemática (MOVIE).
  int running = -1;
  if (scene == kSceneStage && gmode == 3) {
    // el nivel se lee al empezar y se mantiene (con LOOP la estructura ya apunta a la siguiente)
    if (g_active >= 0 && g_active != kMovie) running = g_active;
    else running = lookup_stage(m.read8(kSgi + 12), m.read8(kSgi + 13));
    if (running < 0) running = 99;
  } else if (scene == kSceneMovie) {
    running = kMovie;
  }
  if (running != g_active) {
    const double t = secs();
    if (g_active >= 0)
      std::printf("[DEMO] end %s at %.1f (%.1f s)\n", name_of(g_active), t, t - g_active_t);
    if (running >= 0) {
      std::printf("[DEMO] start %s at %.1f\n", name_of(running), t);
      if (g_force) {
        if (running == g_list[g_pos]) {
          if (++g_pos == g_list.size()) {
            g_pos = 0;
            std::printf("[DEMO] lista completa; vuelve a empezar\n");
          }
        } else {
          std::printf("[DEMO] aviso: se esperaba %s\n", name_of(g_list[g_pos]));
        }
      }
    }
    std::fflush(stdout);
    g_active = running;
    g_active_t = t;
    g_tprof_scene.store(running, std::memory_order_relaxed);
  }
  // NWII_FORCE_LEVEL: la primera vez que el arranque pide la pantalla de título (startTitle(0):
  // mundo 1, STAGE_TITLE=39, modo TITLE) se cambia por un nivel en modo normal, como haría
  // reStartPeachCastle con otro nivel. Solo una vez; luego el juego sigue su curso normal.
  if (g_lvl_w >= 0 && !g_lvl_done && m.read32(kSgi + 8) == 2 && m.read8(kSgi + 13) == 39) {
    m.write8(kSgi + 7, 0);
    m.write32(kSgi + 8, 0);
    m.write8(kSgi + 12, (uint8_t)g_lvl_w);
    m.write8(kSgi + 13, (uint8_t)g_lvl_s);
    m.write8(kSgi + 14, (uint8_t)g_lvl_w);
    m.write8(kSgi + 15, (uint8_t)g_lvl_s);
    g_lvl_done = true;
    // Jugador 1 = Mario, como deja la selección de archivo para 1 jugador (sin esto el nivel
    // arranca sin nadie). daPyMng_c: mPlayerEntry[4] 0x80354E50, mPlayerType[4] 0x80354E60,
    // mRest[4] (por tipo) 0x80354E90.
    std::printf("[DEMO] antes: entry %u %u %u %u type %u %u %u %u rest %d %d %d %d\n",
                m.read32(0x80354E50), m.read32(0x80354E54), m.read32(0x80354E58), m.read32(0x80354E5C),
                m.read32(0x80354E60), m.read32(0x80354E64), m.read32(0x80354E68), m.read32(0x80354E6C),
                (int)m.read32(0x80354E90), (int)m.read32(0x80354E94), (int)m.read32(0x80354E98),
                (int)m.read32(0x80354E9C));
    m.write32(0x80354E50, 1);
    m.write32(0x80354E60, 0);
    if ((int32_t)m.read32(0x80354E90) <= 0) m.write32(0x80354E90, 5);
    std::printf("[DEMO] arranque directo: título -> nivel %d-%d (tick %u, escena %d)\n", g_lvl_w + 1,
                g_lvl_s + 1, g_ticks, scene);
  }
  if (!g_force) return;

  // 2) Forzar la siguiente elección.
  if (!g_table_ready) {  // no tocar nada hasta que el juego haya barajado la tabla (dScBoot)
    unsigned seen = 0;
    for (int i = 0; i < 15; ++i) { uint8_t v = m.read8(kTable + i); if (v < 15) seen |= 1u << v; }
    if (seen != 0x7FFF) return;
    g_table_ready = true;
    if (g_log >= 2) std::printf("[DEMO] tabla barajada vista (tick %u)\n", g_ticks);
  }
  const int next = g_list[g_pos];
  if (next < kMovie) {
    const uint32_t tc = m.read32(kTitleCount);
    if (tc < 15 && m.read8(kTable + tc) != next) m.write8(kTable + tc, (uint8_t)next);
  }
  // Demo o cinemática: la pantalla de título decide con (contador % 4); se ajusta solo mientras
  // se está en ella (modo TITLE), antes de los 900 frames.
  if (scene == kSceneStage && gmode == 2) {
    const int32_t mc = (int32_t)m.read32(kMovieCount);
    const bool movie = (mc % 4) == 0;
    if (movie != (next == kMovie)) {
      const int32_t nv = (next == kMovie) ? (mc - (mc % 4)) : (mc - (mc % 4) + 1);
      m.write32(kMovieCount, (uint32_t)nv);
      if (g_log >= 2) std::printf("[DEMO] contador demo/MOVIE %d -> %d\n", mc, nv);
    }
  }
  // 3) LOOP: al acabar una demo por tiempo (modo TITLE_REPLAY, más de 2400 frames) el juego llama a
  // startTitle(0) -> pantalla de título. Se cambia lo que esa llamada escribió por lo que habría
  // escrito startTitle(1) con la siguiente demo (las dos ramas solo difieren en estos campos; el
  // fundido y el cambio de escena son los mismos). Salir de la demo con un botón no se toca.
  if (g_loop && next < kMovie && scene == kSceneStage && gmode == 3 && exef > 2400 &&
      m.read32(kSgi + 8) == 2) {
    m.write8(kSgi + 7, 1);
    m.write32(kSgi + 8, 3);
    m.write8(kSgi + 12, kDemos[next].world);
    m.write8(kSgi + 13, kDemos[next].stage);
    m.write8(kSgi + 14, kDemos[next].world);
    m.write8(kSgi + 15, kDemos[next].stage);
    static uint32_t n = 0;
    if (g_log >= 2 || n++ < 1) std::printf("[DEMO] sin título: fin de demo -> %s (tick %u)\n", name_of(next), g_ticks);
  }
  (void)kStageAddr;
}

}  // namespace nwii::runtime
