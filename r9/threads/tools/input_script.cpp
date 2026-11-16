// Test-only scripted Wii Remote input for offscreen / citron runs (R9 base).
// Superset of cpu/tools/input_script.cpp (same NWII_INPUT_SCRIPT syntax), plus
// a scene log and scene-driven phases, and it also builds for the Switch.
// Linked into separate test binaries with -Wl,--wrap=NWII_KPADRead,
// --wrap=NWII_KPADReadEx (r9/base/build_pc.sh -> <out>_input_test,
// r9/base/build_nx.sh -> <out>_input_test.nro); the shipped runtime
// (wpad_hle.cpp) is untouched. Never ship these binaries.
//
// NWII_INPUT_SCRIPT = segment[;segment...]          (phase 0, from boot)
//   segment  = [pN:]from-to:mask:period:len
//     frames (KPAD reads on channel 0) in [from, to) press `mask` (hex WPAD
//     button bits) for player N (1-4, default 1) while
//     (frame - from) % period < len.
//   event    = pN:on@F | pN:off@F   connect / disconnect player N's
//     controller at frame F (N >= 2). A player 2-4 that appears in the script
//     without an on@ event is connected from frame 0.
// WPAD bits: LEFT 0x1 RIGHT 0x2 DOWN 0x4 UP 0x8 PLUS 0x10 2 0x100 1 0x200
//   B 0x400 A 0x800 MINUS 0x1000 HOME 0x8000. Held sideways: screen-right =
//   DOWN (0x4), screen-left = UP (0x8). Extra bits for players 2-4:
//   0x10000 shake, 0x20000 tilt left, 0x40000 tilt right.
//
// Scenes (new in R9): every channel-0 read looks at dScene_c::m_nowScene
// (u16 at NWII_INPUT_SCENE_ADDR, default 0x80428732 = r13 0x8042F680 - 0x6F4E,
// from createNextScene 0x800E1DE0 of SMNE01) and prints
//   [SCENE] frame=F t=S scene=N (NAME) prev=M phase=P
// to stdout when it changes (S = seconds since the first KPAD read). Profile
// ids: 0 BOOT 1 AUTO_SELECT 2 SELECT 3 WORLD_MAP 4 WORLD_9_DEMO 5 STAGE
// 6 RESTART_CRSIN 7 CRSIN 8 MOVIE 9 GAMEOVER 10 GAME_SETUP. The title screen
// is a STAGE too (the title course), entered through RESTART_CRSIN.
// NWII_INPUT_PHASES = "A>B>..." (decimal scene ids): phase k (1..) starts the
//   first time m_nowScene becomes the k-th id, after phase k-1 started. From
//   then on the buttons come from NWII_INPUT_SCRIPT_<k> (same syntax, frames
//   counted from the start of the phase; empty or unset = no buttons). The
//   connection events of phase 0 stay as they are.
//   e.g. "3" + no SCRIPT_1: press through the menus, stop on the World map.
// NWII_INPUT_PHASE_DELAY = n: a phase starts n frames after its scene appears
//   (default 0); buttons of the old phase stop at once.
//
// Player 1: the buttons are ORed into the KPAD status of channel 0 after the
// real HLE filled it (trigger/release recomputed), as before.
// Players 2-4 (PC, default): each is a virtual SDL game controller
// (SDL_JoystickAttachVirtualEx) that the runtime picks up through its normal
// hotplug + per-player mapping (use [input.player1] device = "keyboard" so
// player 1 does not take the first one). NWII_INPUT_DIRECT=1, no SDL joystick
// subsystem, or the Switch build: the channel is forced connected in the
// runtime and the buttons are ORed into its KPAD status like player 1.
// NWII_INPUT_LOG=1 prints the frame number every 600 reads, rumble changes.
// NWII_INPUT_PEEK="[*]BASE:off,off,...[@every]" (hex): print guest words.
#include "runtime/cpu_context.h"
#include "input/wii_remotes.h"

#if !defined(__SWITCH__)
#define NWII_INPUT_SDL 1
#include <SDL.h>
#else
#define NWII_INPUT_SDL 0
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using nwii::runtime::CPUContext;
namespace inp = nwii::runtime::input;

extern "C" void __real_NWII_KPADRead(CPUContext &ctx);
extern "C" void __real_NWII_KPADReadEx(CPUContext &ctx);

namespace {

constexpr int kPlayers = 4;
constexpr unsigned kShake = 0x10000, kTiltL = 0x20000, kTiltR = 0x40000;
constexpr int kMaxPhases = 8;

struct Seg {
  int player;
  unsigned from, to, mask, period, len;
};
struct Ev {
  int player;
  unsigned frame;
  bool on;
};

struct Script {
  std::vector<Seg> segs;
  std::vector<Ev> evs;
  bool used[kPlayers] = {};
};

Script parse(const char *e, const char *what) {
  Script sc;
  if (!e) return sc;
  const char *p = e;
  bool explicit_on[kPlayers] = {};
  while (*p) {
    int player = 1;
    const char *q = p;
    int consumed = 0;
    if ((q[0] == 'p' || q[0] == 'P') && std::sscanf(q + 1, "%d:%n", &player, &consumed) == 1 &&
        consumed > 0)
      q += 1 + consumed;
    if (player < 1 || player > kPlayers) player = 1;
    unsigned f = 0;
    Seg s{};
    if (std::sscanf(q, "on@%u", &f) == 1) {
      sc.evs.push_back({player - 1, f, true});
      explicit_on[player - 1] = true;
      sc.used[player - 1] = true;
    } else if (std::sscanf(q, "off@%u", &f) == 1) {
      sc.evs.push_back({player - 1, f, false});
      sc.used[player - 1] = true;
    } else if (std::sscanf(q, "%u-%u:%x:%u:%u", &s.from, &s.to, &s.mask, &s.period,
                           &s.len) == 5 && s.period > 0) {
      s.player = player - 1;
      sc.segs.push_back(s);
      sc.used[player - 1] = true;
    }
    const char *n = std::strchr(p, ';');
    if (!n) break;
    p = n + 1;
  }
  for (int i = 1; i < kPlayers; ++i)
    if (sc.used[i] && !explicit_on[i]) sc.evs.push_back({i, 0, true});
  std::fprintf(stderr, "[INPUT-SCRIPT] %s: %zu segments, %zu connection events\n", what,
               sc.segs.size(), sc.evs.size());
  return sc;
}

// Phase 0 = NWII_INPUT_SCRIPT; phase k = NWII_INPUT_SCRIPT_<k>.
struct Phases {
  Script script[kMaxPhases + 1];
  int scene[kMaxPhases + 1] = {};  // scene id that starts phase k (k >= 1)
  int count = 0;                   // number of phases after phase 0
  unsigned delay = 0;
  uint32_t scene_addr = 0x80428732;
};

const Phases &phases() {
  static const Phases ph = [] {
    Phases r;
    r.script[0] = parse(std::getenv("NWII_INPUT_SCRIPT"), "phase 0");
    if (const char *a = std::getenv("NWII_INPUT_SCENE_ADDR"))
      r.scene_addr = (uint32_t)std::strtoul(a, nullptr, 16);
    if (const char *d = std::getenv("NWII_INPUT_PHASE_DELAY")) r.delay = (unsigned)std::atoi(d);
    if (const char *s = std::getenv("NWII_INPUT_PHASES")) {
      const char *p = s;
      while (*p && r.count < kMaxPhases) {
        char *end = nullptr;
        long v = std::strtol(p, &end, 10);
        if (end == p) break;
        ++r.count;
        r.scene[r.count] = (int)v;
        std::string key = "NWII_INPUT_SCRIPT_" + std::to_string(r.count);
        std::string what = "phase " + std::to_string(r.count) + " (scene " + std::to_string(v) + ")";
        r.script[r.count] = parse(std::getenv(key.c_str()), what.c_str());
        p = end;
        while (*p == '>' || *p == ',' || *p == ' ') ++p;
      }
    }
    return r;
  }();
  return ph;
}

unsigned g_frame = 0;
int g_phase = 0;
unsigned g_phase_start = 0;   // frame the current phase started
int g_pending = 0;            // phase waiting for its delay (0 = none)
unsigned g_pending_at = 0;
int g_scene = -1;
unsigned g_prev[kPlayers] = {};
unsigned g_mask[kPlayers] = {};  // this frame's scripted buttons
bool g_on[kPlayers] = {};        // scripted controller connected
bool g_direct = false;

const char *scene_name(int s) {
  static const char *names[] = {"BOOT",  "AUTO_SELECT",   "SELECT", "WORLD_MAP",
                                "WORLD_9_DEMO", "STAGE", "RESTART_CRSIN", "CRSIN",
                                "MOVIE", "GAMEOVER",      "GAME_SETUP",
                                "MULTI_PLAY_COURSE_SELECT"};
  if (s >= 0 && s < (int)(sizeof names / sizeof names[0])) return names[s];
  return s == 750 ? "INVALID" : "?";
}

double seconds() {
  using clk = std::chrono::steady_clock;
  static const clk::time_point t0 = clk::now();
  return std::chrono::duration<double>(clk::now() - t0).count();
}

#if NWII_INPUT_SDL
// --- virtual SDL controllers for players 2-4 (PC) ---
struct VPad {
  SDL_Joystick *joy = nullptr;
  SDL_JoystickID id = -1;
  int player = 0;
  int rumble_lo = -1, rumble_hi = -1;
};
VPad g_vpad[kPlayers];

int SDLCALL vpad_rumble(void *ud, Uint16 lo, Uint16 hi) {
  VPad *v = (VPad *)ud;
  if (v->rumble_lo != lo || v->rumble_hi != hi) {
    std::fprintf(stderr, "[INPUT-SCRIPT] frame=%u P%d rumble %u/%u\n", g_frame,
                 v->player + 1, lo, hi);
    v->rumble_lo = lo;
    v->rumble_hi = hi;
  }
  return 0;
}

bool vpad_attach(int p) {
  VPad &v = g_vpad[p];
  if (v.joy) return true;
  SDL_VirtualJoystickDesc d;
  std::memset(&d, 0, sizeof d);
  d.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
  d.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
  d.naxes = SDL_CONTROLLER_AXIS_MAX;
  d.nbuttons = SDL_CONTROLLER_BUTTON_DPAD_RIGHT + 1;
  d.button_mask = (1u << d.nbuttons) - 1;
  d.axis_mask = (1u << SDL_CONTROLLER_AXIS_MAX) - 1;
  static char names[kPlayers][32];
  std::snprintf(names[p], sizeof names[p], "Script Pad P%d", p + 1);
  d.name = names[p];
  // Distinct GUIDs: SDL names a controller after the mapping of its GUID, so
  // identical ones would all show the first pad's name.
  d.vendor_id = 0x1209;
  d.product_id = (Uint16)(0xB000 + p);
  d.userdata = &v;
  d.Rumble = vpad_rumble;
  v.player = p;
  const int idx = SDL_JoystickAttachVirtualEx(&d);
  if (idx < 0) {
    std::fprintf(stderr, "[INPUT-SCRIPT] P%d: SDL_JoystickAttachVirtualEx failed: %s\n",
                 p + 1, SDL_GetError());
    return false;
  }
  v.joy = SDL_JoystickOpen(idx);
  if (!v.joy) return false;
  v.id = SDL_JoystickInstanceID(v.joy);
  // Triggers rest at -32768 on a full-range virtual axis (= released).
  SDL_JoystickSetVirtualAxis(v.joy, SDL_CONTROLLER_AXIS_TRIGGERLEFT, -32768);
  SDL_JoystickSetVirtualAxis(v.joy, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, -32768);
  std::fprintf(stderr, "[INPUT-SCRIPT] frame=%u P%d: virtual controller attached\n",
               g_frame, p + 1);
  return true;
}

void vpad_detach(int p) {
  VPad &v = g_vpad[p];
  if (!v.joy) return;
  const int n = SDL_NumJoysticks();
  int idx = -1;
  for (int i = 0; i < n; ++i)
    if (SDL_JoystickGetDeviceInstanceID(i) == v.id) idx = i;
  SDL_JoystickClose(v.joy);
  v.joy = nullptr;
  if (idx >= 0) SDL_JoystickDetachVirtual(idx);
  std::fprintf(stderr, "[INPUT-SCRIPT] frame=%u P%d: virtual controller detached\n",
               g_frame, p + 1);
}

void vpad_set(int p, unsigned m) {
  SDL_Joystick *j = g_vpad[p].joy;
  if (!j) return;
  static unsigned last[kPlayers];
  static const bool log = std::getenv("NWII_INPUT_LOG") != nullptr;
  if (log && m != last[p])
    std::fprintf(stderr, "[INPUT-SCRIPT] frame=%u P%d mask=%x\n", g_frame, p + 1, m);
  last[p] = m;
  auto btn = [j](int b, bool on) { SDL_JoystickSetVirtualButton(j, b, on ? 1 : 0); };
  btn(SDL_CONTROLLER_BUTTON_A, m & 0x100);
  btn(SDL_CONTROLLER_BUTTON_X, m & 0x200);
  btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, m & 0x800);
  btn(SDL_CONTROLLER_BUTTON_START, m & 0x10);
  btn(SDL_CONTROLLER_BUTTON_BACK, m & 0x1000);
  btn(SDL_CONTROLLER_BUTTON_GUIDE, m & 0x8000);
  btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT, m & 0x8);   // WPAD UP = screen-left
  btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, m & 0x4);  // WPAD DOWN = screen-right
  btn(SDL_CONTROLLER_BUTTON_DPAD_UP, m & 0x2);     // WPAD RIGHT = screen-up
  btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN, m & 0x1);   // WPAD LEFT = screen-down
  btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, m & kShake);
  SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_TRIGGERLEFT, (m & 0x400) ? 32767 : -32768);
  SDL_JoystickSetVirtualAxis(j, SDL_CONTROLLER_AXIS_RIGHTX,
                             (m & kTiltL) ? -32767 : (m & kTiltR) ? 32767 : 0);
}
bool sdl_joystick_up() { return SDL_WasInit(SDL_INIT_JOYSTICK) != 0; }
#else
bool vpad_attach(int) { return false; }
void vpad_detach(int) {}
void vpad_set(int, unsigned) {}
bool sdl_joystick_up() { return false; }
#endif

// Scene watch + phase switch, once per game frame (channel 0, before the read).
void scene_tick(CPUContext &ctx) {
  const Phases &ph = phases();
  const int now = (int)ctx.mmu.read16(ph.scene_addr);
  if (now != g_scene) {
    std::printf("[SCENE] frame=%u t=%.1f scene=%d (%s) prev=%d phase=%d\n", g_frame, seconds(),
                now, scene_name(now), g_scene, g_phase);
    std::fflush(stdout);
    if (g_phase < ph.count && now == ph.scene[g_phase + 1] && !g_pending) {
      g_pending = g_phase + 1;
      g_pending_at = g_frame + ph.delay;
    }
    g_scene = now;
  }
  if (g_pending && g_frame >= g_pending_at) {
    g_phase = g_pending;
    g_pending = 0;
    g_phase_start = g_frame;
    std::printf("[SCENE] frame=%u t=%.1f phase %d starts (scene %d)\n", g_frame, seconds(),
                g_phase, g_scene);
    std::fflush(stdout);
  }
}

// Once per game frame (channel 0 read): connection events, this frame's
// masks, virtual controller state.
void tick(CPUContext &ctx) {
  const Phases &ph = phases();
  static const bool log = std::getenv("NWII_INPUT_LOG") != nullptr;
  static bool init = false;
  if (!init) {
    init = true;
    g_direct = std::getenv("NWII_INPUT_DIRECT") != nullptr || !sdl_joystick_up();
    if (g_direct)
      std::fprintf(stderr, "[INPUT-SCRIPT] players 2-4 injected directly\n");
    seconds();
  }
  scene_tick(ctx);
  for (const Ev &e : ph.script[0].evs) {
    if (e.frame != g_frame || e.player == 0) continue;
    g_on[e.player] = e.on;
    if (g_direct) {
      inp::remote_force_connected(e.player, e.on);
      std::fprintf(stderr, "[INPUT-SCRIPT] frame=%u P%d: %s (direct)\n", g_frame,
                   e.player + 1, e.on ? "connected" : "disconnected");
    } else if (e.on) {
      vpad_attach(e.player);
    } else {
      vpad_detach(e.player);
    }
  }
  for (int p = 0; p < kPlayers; ++p) g_mask[p] = 0;
  // Between a phase's scene change and its start (PHASE_DELAY) no buttons.
  if (!g_pending) {
    const unsigned f = g_frame - g_phase_start;
    for (const Seg &s : ph.script[g_phase].segs)
      if (f >= s.from && f < s.to && (f - s.from) % s.period < s.len)
        g_mask[s.player] |= s.mask;
  }
  if (!g_direct)
    for (int p = 1; p < kPlayers; ++p)
      if (g_on[p]) vpad_set(p, g_mask[p]);
  if (log && g_frame % 600 == 0)
    std::fprintf(stderr, "[INPUT-SCRIPT] frame=%u phase=%d masks=%x/%x/%x/%x connected=%x\n",
                 g_frame, g_phase, g_mask[0], g_mask[1], g_mask[2], g_mask[3],
                 inp::remote_connected_mask());
}

void apply(CPUContext &ctx, uint32_t chan, uint32_t buf) {
  if (chan >= (uint32_t)kPlayers || !buf || ctx.gpr[3] == 0) return;
  if (chan == 0 || g_direct) {
    const unsigned add = g_mask[chan] & 0xFFFF;
    uint32_t hold = ctx.mmu.read32(buf) | add;
    ctx.mmu.write32(buf + 0x00, hold);
    ctx.mmu.write32(buf + 0x04, hold & ~g_prev[chan]);
    ctx.mmu.write32(buf + 0x08, ~hold & g_prev[chan]);
    g_prev[chan] = hold;
  }
  if (chan == 0) ++g_frame;
}

// NWII_INPUT_PEEK="[*]BASE:off,off,...[@every]" (hex): print the guest words at
// BASE+off (BASE dereferenced first with *) every `every` frames (default 60).
void peek(CPUContext &ctx) {
  static const char *spec = std::getenv("NWII_INPUT_PEEK");
  if (!spec) return;
  static unsigned every = 0, base = 0;
  static bool deref = false;
  static std::vector<unsigned> offs;
  if (!every) {
    const char *p = spec;
    if (*p == '*') { deref = true; ++p; }
    base = (unsigned)std::strtoul(p, (char **)&p, 16);
    while (*p == ':' || *p == ',') offs.push_back((unsigned)std::strtoul(p + 1, (char **)&p, 16));
    every = *p == '@' ? (unsigned)std::atoi(p + 1) : 60;
    if (!every) every = 60;
  }
  if (g_frame % every) return;
  uint32_t b = deref ? ctx.mmu.read32(base) : base;
  std::fprintf(stderr, "[PEEK] frame=%u base=%08x", g_frame, b);
  if (b >= 0x80000000u && b < 0x81800000u)
    for (unsigned o : offs) std::fprintf(stderr, " +%x=%08x", o, ctx.mmu.read32(b + o));
  std::fprintf(stderr, "\n");
}

void before(CPUContext &ctx, uint32_t chan) {
  if (chan == 0) {
    tick(ctx);
    peek(ctx);
  }
}

}  // namespace

extern "C" void __wrap_NWII_KPADRead(CPUContext &ctx) {
  const uint32_t chan = ctx.gpr[3], buf = ctx.gpr[4];
  before(ctx, chan);
  __real_NWII_KPADRead(ctx);
  apply(ctx, chan, buf);
}

extern "C" void __wrap_NWII_KPADReadEx(CPUContext &ctx) {
  const uint32_t chan = ctx.gpr[3], buf = ctx.gpr[4];
  before(ctx, chan);
  __real_NWII_KPADReadEx(ctx);
  apply(ctx, chan, buf);
}
