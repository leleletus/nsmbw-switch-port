// Virtual Wii Remotes for up to four players (see include/input/wii_remotes.h).
//
// PC: every SDL game controller is opened once by the host thread (hotplug is
// polled each frame), and handed to the first player slot that wants one.
// Default slots: P1 = keyboard + first controller, P2-P4 = the next ones.
// Switch: players come from libnx HID (platform/switch/switch_input.cpp).
#include "input/wii_remotes.h"

#include "runtime/config.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

#ifdef __SWITCH__
#include "../platform/switch/switch_input.h"
#endif

namespace nwii::runtime::input {

namespace {

enum Action {
  A_LEFT, A_RIGHT, A_UP, A_DOWN, A_TWO, A_ONE, A_A, A_B, A_PLUS, A_MINUS,
  A_HOME, A_SHAKE, A_TILT_L, A_TILT_R, A_COUNT
};
const char *const kActionNames[A_COUNT] = {
    "left", "right", "up", "down", "two", "one", "a", "b",
    "plus", "minus", "home", "shake", "tilt_left", "tilt_right"};

constexpr float kMaxTilt = 1.0f;          // radians at full stick / key
constexpr float kStickDir = 0.5f;         // left stick -> d-pad threshold
constexpr float kStickTiltDead = 0.25f;   // right stick -> tilt dead zone
constexpr float kTriggerOn = 0.5f;

// A controller input bound to an action: a button, or an axis past a
// threshold (triggers; sticks with a sign).
struct PadBinding {
  int button = -1;  // SDL_GameControllerButton
  int axis = -1;    // SDL_GameControllerAxis
  int sign = 1;     // axis direction that counts
};

enum class TiltMode { Auto, Stick, Motion, None };

struct PlayerMap {
  bool enabled = false;
  bool use_keyboard = false;
  bool use_pad = false;
  std::string selector;
  std::vector<SDL_Scancode> keys[A_COUNT];
  std::vector<PadBinding> pad[A_COUNT];
  TiltMode tilt = TiltMode::Auto;
  bool tilt_invert = false;
  bool rumble = true;
};

struct HostPad {
  SDL_GameController *gc = nullptr;
  SDL_JoystickID id = -1;
  int order = 0;  // connection order, "#n" selectors
  std::string name, guid;
  int player = -1;
  bool accel = false;
};

// Shared with the CPU thread.
std::mutex g_mu;
RemoteInput g_snap[kMaxRemotes];
std::string g_desc[kMaxRemotes];
bool g_published = false;
std::atomic<bool> g_motor[kMaxRemotes];
std::atomic<unsigned> g_forced{0};
std::atomic<bool> g_accepting{false};

// Host thread only.
bool g_inited = false;
PlayerMap g_map[kMaxRemotes];
std::vector<HostPad> g_pads;
int g_pad_order = 0;
bool g_rumbling[kMaxRemotes] = {};
bool g_was_connected[kMaxRemotes] = {};

bool env_flag(const char *name) {
  const char *v = std::getenv(name);
  return v && *v && *v != '0';
}

std::string lower(std::string s) {
  for (char &c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

void add_keys(PlayerMap &m, Action a, std::initializer_list<const char *> names) {
  for (const char *n : names) {
    SDL_Scancode sc = SDL_GetScancodeFromName(n);
    if (sc != SDL_SCANCODE_UNKNOWN) m.keys[a].push_back(sc);
  }
}

bool parse_pad_binding(const std::string &raw, PadBinding *b) {
  std::string n = lower(raw);
  int sign = 1;
  if (!n.empty() && (n.back() == '+' || n.back() == '-')) {
    sign = n.back() == '-' ? -1 : 1;
    n.pop_back();
  }
  SDL_GameControllerButton btn = SDL_GameControllerGetButtonFromString(n.c_str());
  if (btn != SDL_CONTROLLER_BUTTON_INVALID) {
    b->button = btn;
    return true;
  }
  SDL_GameControllerAxis ax = SDL_GameControllerGetAxisFromString(n.c_str());
  if (ax != SDL_CONTROLLER_AXIS_INVALID) {
    b->axis = ax;
    b->sign = sign;
    return true;
  }
  return false;
}

void add_pad(PlayerMap &m, Action a, std::initializer_list<const char *> names) {
  for (const char *n : names) {
    PadBinding b;
    if (parse_pad_binding(n, &b)) m.pad[a].push_back(b);
  }
}

// Default mapping, the Wii Remote held sideways (d-pad under the left thumb):
//   keyboard P1: arrows, X = 2 (jump), Z = 1 (run), A = A, S = B,
//     Enter = +, Backspace = -, H = HOME, C = shake, Q / E = tilt
//   keyboard P2 (only if [input.player2] device includes the keyboard):
//     I J K L, Space = 2, V = 1, G = A, T = B, P = +, O = -, F = shake,
//     Y / U = tilt
//   controller: d-pad / left stick, A or B (bottom / right) = 2,
//     X or Y (left / top) = 1, LB = A, LT = B, Start = +, Back = -,
//     Guide = HOME, RB or RT = shake, right stick X or motion = tilt
void default_keys(PlayerMap &m, int player) {
  if (player == 0) {
    add_keys(m, A_LEFT, {"Left"});
    add_keys(m, A_RIGHT, {"Right"});
    add_keys(m, A_UP, {"Up"});
    add_keys(m, A_DOWN, {"Down"});
    add_keys(m, A_TWO, {"X"});
    add_keys(m, A_ONE, {"Z"});
    add_keys(m, A_A, {"A"});
    add_keys(m, A_B, {"S"});
    add_keys(m, A_PLUS, {"Return"});
    add_keys(m, A_MINUS, {"Backspace"});
    add_keys(m, A_HOME, {"H"});
    add_keys(m, A_SHAKE, {"C"});
    add_keys(m, A_TILT_L, {"Q"});
    add_keys(m, A_TILT_R, {"E"});
  } else if (player == 1) {
    add_keys(m, A_LEFT, {"J"});
    add_keys(m, A_RIGHT, {"L"});
    add_keys(m, A_UP, {"I"});
    add_keys(m, A_DOWN, {"K"});
    add_keys(m, A_TWO, {"Space"});
    add_keys(m, A_ONE, {"V"});
    add_keys(m, A_A, {"G"});
    add_keys(m, A_B, {"T"});
    add_keys(m, A_PLUS, {"P"});
    add_keys(m, A_MINUS, {"O"});
    add_keys(m, A_SHAKE, {"F"});
    add_keys(m, A_TILT_L, {"Y"});
    add_keys(m, A_TILT_R, {"U"});
  }
}

void default_pad(PlayerMap &m) {
  add_pad(m, A_LEFT, {"dpleft"});
  add_pad(m, A_RIGHT, {"dpright"});
  add_pad(m, A_UP, {"dpup"});
  add_pad(m, A_DOWN, {"dpdown"});
  add_pad(m, A_TWO, {"a", "b"});
  add_pad(m, A_ONE, {"x", "y"});
  add_pad(m, A_A, {"leftshoulder"});
  add_pad(m, A_B, {"lefttrigger"});
  add_pad(m, A_PLUS, {"start"});
  add_pad(m, A_MINUS, {"back"});
  add_pad(m, A_HOME, {"guide"});
  add_pad(m, A_SHAKE, {"rightshoulder", "righttrigger"});
}

void build_maps() {
  const Config &cfg = Config::get();
  for (int p = 0; p < kMaxRemotes; ++p) {
    PlayerMap &m = g_map[p];
    m = PlayerMap{};
    const InputPlayerConfig &pc = cfg.input_player[p];
    m.enabled = p < cfg.input_players;
    const std::string dev = lower(pc.device);
    if (dev == "auto" || dev.empty()) {
      m.use_keyboard = p == 0;
      m.use_pad = true;
    } else if (dev == "none" || dev == "off") {
      m.enabled = p == 0 && m.enabled;  // player 1 stays connected
    } else {
      m.use_keyboard = dev.find("keyboard") != std::string::npos ||
                       dev.find("teclado") != std::string::npos;
      m.use_pad = dev.find("pad") != std::string::npos ||
                  dev.find("controller") != std::string::npos ||
                  dev.find("mando") != std::string::npos ||
                  dev.find("joy") != std::string::npos;
      if (!m.use_keyboard && !m.use_pad)
        std::cerr << "[WPAD] P" << (p + 1) << ": unknown device '" << pc.device
                  << "' (auto|keyboard|gamepad|keyboard+gamepad|none)" << std::endl;
    }
    m.selector = pc.gamepad;
    const std::string tilt = lower(pc.tilt);
    m.tilt = tilt == "stick"    ? TiltMode::Stick
             : tilt == "motion" || tilt == "gyro" || tilt == "accel"
                 ? TiltMode::Motion
             : tilt == "none" || tilt == "off" ? TiltMode::None
                                               : TiltMode::Auto;
    m.tilt_invert = pc.tilt_invert;
    m.rumble = pc.rumble;

    default_keys(m, p);
    default_pad(m);
    for (int a = 0; a < A_COUNT; ++a) {
      auto k = pc.keys.find(kActionNames[a]);
      if (k != pc.keys.end()) {
        m.keys[a].clear();
        for (const std::string &n : k->second) {
          SDL_Scancode sc = SDL_GetScancodeFromName(n.c_str());
          if (sc == SDL_SCANCODE_UNKNOWN)
            std::cerr << "[WPAD] P" << (p + 1) << ": unknown key '" << n << "'" << std::endl;
          else
            m.keys[a].push_back(sc);
        }
      }
      auto b = pc.buttons.find(kActionNames[a]);
      if (b != pc.buttons.end()) {
        m.pad[a].clear();
        for (const std::string &n : b->second) {
          PadBinding pb;
          if (parse_pad_binding(n, &pb))
            m.pad[a].push_back(pb);
          else
            std::cerr << "[WPAD] P" << (p + 1) << ": unknown button '" << n << "'" << std::endl;
        }
      }
    }
    if (p >= 2 && m.use_keyboard && pc.keys.empty())
      std::cerr << "[WPAD] P" << (p + 1)
                << ": keyboard without [input.player" << (p + 1)
                << ".keys] has no keys" << std::endl;
  }
}

void init_once() {
  if (g_inited) return;
  g_inited = true;
  build_maps();
  if (const char *f = std::getenv("NWII_WPAD_PLAYERS")) {
    int n = std::atoi(f);
    for (int i = 1; i < n && i < kMaxRemotes; ++i) remote_force_connected(i, true);
  }
#ifdef __SWITCH__
  nx::input_init();
#endif
}

bool selector_matches(const std::string &sel, const HostPad &pad) {
  if (sel.empty()) return true;
  if (sel[0] == '#') return std::atoi(sel.c_str() + 1) == pad.order;
  if (lower(sel) == lower(pad.guid)) return true;
  return lower(pad.name).find(lower(sel)) != std::string::npos;
}

// Hotplug: drop detached controllers, open new ones, give free controllers
// to the players that want one (players with a selector first).
void scan_pads() {
  if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) return;
  for (auto it = g_pads.begin(); it != g_pads.end();) {
    if (SDL_GameControllerGetAttached(it->gc)) {
      ++it;
      continue;
    }
    std::cout << "[WPAD] controller removed: " << it->name;
    if (it->player >= 0) std::cout << " (was P" << (it->player + 1) << ")";
    std::cout << std::endl;
    SDL_GameControllerClose(it->gc);
    it = g_pads.erase(it);
  }
  const int n = SDL_NumJoysticks();
  for (int i = 0; i < n; ++i) {
    if (!SDL_IsGameController(i)) continue;
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
    bool known = false;
    for (const HostPad &p : g_pads) known |= p.id == id;
    if (known) continue;
    SDL_GameController *gc = SDL_GameControllerOpen(i);
    if (!gc) continue;
    HostPad hp;
    hp.gc = gc;
    hp.id = id;
    hp.order = g_pad_order++;
    const char *nm = SDL_GameControllerName(gc);
    hp.name = nm ? nm : "?";
    char guid[64];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof guid);
    hp.guid = guid;
    if (SDL_GameControllerHasSensor(gc, SDL_SENSOR_ACCEL) &&
        SDL_GameControllerSetSensorEnabled(gc, SDL_SENSOR_ACCEL, SDL_TRUE) == 0)
      hp.accel = true;
    std::cout << "[WPAD] controller connected: " << hp.name << " (#" << hp.order
              << (hp.accel ? ", motion" : "") << ")" << std::endl;
    g_pads.push_back(hp);
  }
#ifdef __SWITCH__
  return;  // players come from libnx; SDL pads are only kept open
#endif
  for (int pass = 0; pass < 2; ++pass) {
    for (int p = 0; p < kMaxRemotes; ++p) {
      const PlayerMap &m = g_map[p];
      if (!m.enabled || !m.use_pad || m.selector.empty() != (pass == 1)) continue;
      bool has = false;
      for (const HostPad &hp : g_pads) has |= hp.player == p;
      if (has) continue;
      for (HostPad &hp : g_pads) {
        if (hp.player >= 0 || !selector_matches(m.selector, hp)) continue;
        hp.player = p;
        std::cout << "[WPAD] P" << (p + 1) << " <- " << hp.name << std::endl;
        break;
      }
    }
  }
}

HostPad *pad_of(int player) {
  for (HostPad &hp : g_pads)
    if (hp.player == player) return &hp;
  return nullptr;
}

uint32_t sideways_dpad(bool left, bool right, bool up, bool down) {
  // The physical d-pad is rotated 90 degrees: screen-left is WPAD UP and
  // screen-up is WPAD RIGHT.
  uint32_t b = 0;
  if (left) b |= wpad_btn::UP;
  if (right) b |= wpad_btn::DOWN;
  if (up) b |= wpad_btn::RIGHT;
  if (down) b |= wpad_btn::LEFT;
  return b;
}

constexpr uint32_t kActionBits[A_COUNT] = {
    0, 0, 0, 0, wpad_btn::TWO, wpad_btn::ONE, wpad_btn::A, wpad_btn::B,
    wpad_btn::PLUS, wpad_btn::MINUS, wpad_btn::HOME, 0, 0, 0};

RemoteInput read_player(int p, bool forced) {
  RemoteInput in;
  const PlayerMap &m = g_map[p];
  bool act[A_COUNT] = {};
  float tilt_axis = 0.0f;
  bool have_motion = false;
  float motion_tilt = 0.0f;
  bool device = false;

  if (m.enabled && m.use_keyboard && SDL_WasInit(SDL_INIT_VIDEO)) {
    device = true;
    const Uint8 *k = SDL_GetKeyboardState(nullptr);
    for (int a = 0; a < A_COUNT; ++a)
      for (SDL_Scancode sc : m.keys[a]) act[a] |= k[sc] != 0;
  }

  HostPad *hp = m.enabled ? pad_of(p) : nullptr;
  if (hp) {
    device = true;
    SDL_GameController *c = hp->gc;
    auto axis = [c](int a) {
      return SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)a) / 32767.0f;
    };
    for (int a = 0; a < A_COUNT; ++a)
      for (const PadBinding &b : m.pad[a]) {
        if (b.button >= 0)
          act[a] |= SDL_GameControllerGetButton(c, (SDL_GameControllerButton)b.button) != 0;
        else if (b.axis >= 0)
          act[a] |= axis(b.axis) * b.sign > kTriggerOn;
      }
    const float lx = axis(SDL_CONTROLLER_AXIS_LEFTX);
    const float ly = axis(SDL_CONTROLLER_AXIS_LEFTY);
    act[A_LEFT] |= lx < -kStickDir;
    act[A_RIGHT] |= lx > kStickDir;
    act[A_UP] |= ly < -kStickDir;
    act[A_DOWN] |= ly > kStickDir;

    const float rx = axis(SDL_CONTROLLER_AXIS_RIGHTX);
    const bool stick_ok = m.tilt == TiltMode::Auto || m.tilt == TiltMode::Stick;
    const bool motion_ok = m.tilt == TiltMode::Auto || m.tilt == TiltMode::Motion;
    if (stick_ok && std::fabs(rx) > kStickTiltDead) {
      tilt_axis += rx;
    } else if (motion_ok && hp->accel) {
      // Steering-wheel roll from gravity: turning right lowers the right
      // side, which swings the measured "up" towards -X. SDL rotates the
      // axes of a single sideways Joy-Con to this same frame.
      float a[3];
      if (SDL_GameControllerGetSensorData(c, SDL_SENSOR_ACCEL, a, 3) == 0) {
        float g = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (g > 1.0f) {
          have_motion = true;
          motion_tilt = std::asin(std::fmax(-1.0f, std::fmin(1.0f, -a[0] / g)));
        }
      }
    }
  }

#ifdef __SWITCH__
  if (m.enabled) {
    const nx::Pad np = nx::poll_pad(p);
    if (np.connected) device = true;
    act[A_LEFT] |= np.left;
    act[A_RIGHT] |= np.right;
    act[A_UP] |= np.up;
    act[A_DOWN] |= np.down;
    act[A_TWO] |= np.two;
    act[A_ONE] |= np.one;
    act[A_A] |= np.a;
    act[A_B] |= np.b;
    act[A_PLUS] |= np.plus;
    act[A_MINUS] |= np.minus;
    act[A_HOME] |= np.home;
    act[A_SHAKE] |= np.shake;
    if (tilt_axis == 0.0f) tilt_axis = np.stick_tilt;
    if (!have_motion && np.has_motion) {
      have_motion = true;
      motion_tilt = std::fmax(-kMaxTilt, std::fmin(kMaxTilt, np.motion_tilt));
    }
  }
#endif

  if (act[A_TILT_L]) tilt_axis -= 1.0f;
  if (act[A_TILT_R]) tilt_axis += 1.0f;

  for (int a = 0; a < A_COUNT; ++a)
    if (act[a]) in.buttons |= kActionBits[a];
  in.buttons |= sideways_dpad(act[A_LEFT], act[A_RIGHT], act[A_UP], act[A_DOWN]);
  in.shake = act[A_SHAKE];
  if (tilt_axis != 0.0f)
    in.tilt = std::fmax(-1.0f, std::fmin(1.0f, tilt_axis)) * kMaxTilt;
  else if (have_motion)
    in.tilt = motion_tilt;
  if (m.tilt_invert) in.tilt = -in.tilt;

  // Player 1 is always there (as with the single remote before); the others
  // need a device.
  in.connected = p == 0 || (m.enabled && device) || forced;
  if (!in.connected) in = RemoteInput{};
  return in;
}

std::string describe(int p) {
  const PlayerMap &m = g_map[p];
  std::string d;
  if (m.enabled && m.use_keyboard) d = "keyboard";
  if (const HostPad *hp = m.enabled ? pad_of(p) : nullptr)
    d += (d.empty() ? "" : " + ") + hp->name;
#ifdef __SWITCH__
  if (m.enabled) {
    const std::string nd = nx::describe(p);
    if (!nd.empty()) d += (d.empty() ? "" : " + ") + nd;
  }
#endif
  if (d.empty() && (g_forced.load() >> p & 1)) d = "virtual";
  return d;
}

void drive_rumble(int p, bool want) {
  if (!g_map[p].rumble) want = false;
#ifdef __SWITCH__
  nx::rumble(p, want);
#else
  HostPad *hp = pad_of(p);
  if (!hp) {
    g_rumbling[p] = false;
    return;
  }
  if (want) {
    // Refreshed every host frame while the game holds the motor on.
    SDL_GameControllerRumble(hp->gc, 0xC000, 0xC000, 120);
  } else if (g_rumbling[p]) {
    SDL_GameControllerRumble(hp->gc, 0, 0, 0);
  }
#endif
  g_rumbling[p] = want;
}

}  // namespace

void remotes_update() {
  init_once();
  scan_pads();
  RemoteInput snap[kMaxRemotes];
  std::string desc[kMaxRemotes];
  const unsigned forced = g_forced.load();
  for (int p = 0; p < kMaxRemotes; ++p) {
    snap[p] = read_player(p, forced >> p & 1);
    if (snap[p].connected != g_was_connected[p]) {
      g_was_connected[p] = snap[p].connected;
      desc[p] = describe(p);
      std::cout << "[WPAD] remote " << (p + 1)
                << (snap[p].connected ? " connected (" + desc[p] + ")"
                                      : std::string(" disconnected"))
                << std::endl;
    } else {
      desc[p] = describe(p);
    }
    drive_rumble(p, snap[p].connected && g_motor[p].load());
    static uint32_t last_btn[kMaxRemotes];
    static const bool log_btn = env_flag("NWII_WPAD_LOG_BUTTONS");
    if (log_btn && snap[p].buttons != last_btn[p]) {
      std::printf("[WPAD] P%d buttons %04x\n", p + 1, snap[p].buttons);
      last_btn[p] = snap[p].buttons;
    }
  }
  std::lock_guard<std::mutex> lk(g_mu);
  for (int p = 0; p < kMaxRemotes; ++p) {
    g_snap[p] = snap[p];
    g_desc[p] = desc[p];
  }
  g_published = true;
}

void remotes_guest_frame() {
#ifdef __SWITCH__
  nx::run_pending_applet();
#endif
}

RemoteInput remote_poll(int chan) {
  if (chan < 0 || chan >= kMaxRemotes) return RemoteInput{};
  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_published) {
    // No host loop (headless): player 1 plus forced channels only.
    RemoteInput in;
    in.connected = chan == 0 || (g_forced.load() >> chan & 1);
    return in;
  }
  RemoteInput in = g_snap[chan];
  if (!in.connected && (g_forced.load() >> chan & 1)) in.connected = true;
  return in;
}

unsigned remote_connected_mask() {
  unsigned m = 0;
  for (int c = 0; c < kMaxRemotes; ++c)
    if (remote_poll(c).connected) m |= 1u << c;
  return m;
}

void remote_set_motor(int chan, bool on) {
  if (chan >= 0 && chan < kMaxRemotes) g_motor[chan].store(on);
}

void remote_set_accepting(bool accepting) {
  const bool was = g_accepting.exchange(accepting);
  if (was != accepting && env_flag("NWII_WPAD_LOG"))
    std::cout << "[WPAD] game " << (accepting ? "accepts" : "stops accepting")
              << " new remotes" << std::endl;
}

void remote_request_sync() {
  // Called every frame while the sync screen is up: only the first call
  // after a pause of more than a second counts as a new request.
  static uint32_t last_ms = 0;
  const uint32_t now = SDL_GetTicks();
  const bool fresh = last_ms == 0 || now - last_ms > 1000;
  last_ms = now ? now : 1;
  if (!fresh) return;
  std::cout << "[WPAD] game opened its add-a-remote screen" << std::endl;
#ifdef __SWITCH__
  nx::game_wants_remotes();
#endif
}

void remote_force_connected(int chan, bool on) {
  if (chan < 0 || chan >= kMaxRemotes) return;
  if (on)
    g_forced.fetch_or(1u << chan);
  else
    g_forced.fetch_and(~(1u << chan));
}

std::string remote_describe(int chan) {
  if (chan < 0 || chan >= kMaxRemotes) return {};
  std::lock_guard<std::mutex> lk(g_mu);
  return g_desc[chan];
}

}  // namespace nwii::runtime::input
