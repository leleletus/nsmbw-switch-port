#ifdef __SWITCH__
#include "switch_input.h"

#include "runtime/config.h"

#include <switch.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace nwii::runtime::nx {

namespace {

constexpr float kStickDeadZone = 0.25f;
constexpr float kStickDir = 0.5f;
constexpr float kTiltDeadZone = 0.06f;   // radians (~3.5 deg): a resting hand
constexpr float kShakeThresholdG = 2.2f; // |acceleration| that counts as a shake
constexpr int kShakePulseFrames = 8;
constexpr int kShakeCooldownFrames = 12;
constexpr int kAppletHoldFrames = 30;    // hotkey held this long opens the applet

// Styles one player's Npad can be read in.
enum Style { S_HANDHELD, S_FULLKEY, S_JOYDUAL, S_JOYLEFT, S_JOYRIGHT, S_COUNT };
constexpr HidNpadStyleTag kStyleTag[S_COUNT] = {
    HidNpadStyleTag_NpadHandheld, HidNpadStyleTag_NpadFullKey,
    HidNpadStyleTag_NpadJoyDual, HidNpadStyleTag_NpadJoyLeft,
    HidNpadStyleTag_NpadJoyRight};
// hidGetSixAxisSensorHandles only accepts 2 handles for JoyDual (handheld and
// FullKey report one sensor; 2 returns LibnxError_BadInput), while
// hidInitializeVibrationDevices takes 2 (both motors) for handheld/FullKey/dual.
constexpr int kStyleSixAxis[S_COUNT] = {1, 1, 2, 1, 1};
constexpr int kStyleVibration[S_COUNT] = {2, 2, 2, 1, 1};
const char *const kStyleName[S_COUNT] = {"handheld", "Pro Controller",
                                         "Joy-Con pair", "Joy-Con (L)",
                                         "Joy-Con (R)"};

struct Player {
  HidNpadIdType id = HidNpadIdType_No1;
  PadState pad{};
  HidSixAxisSensorHandle six[S_COUNT][2]{};
  bool six_ok[S_COUNT] = {};
  HidVibrationDeviceHandle vib[S_COUNT][2]{};
  bool vib_ok[S_COUNT] = {};
  // motion
  float last_tilt = 0.0f;
  int shake_pulse = 0;
  int shake_cooldown = 0;
  bool was_connected = false;
  uint64_t lost_tick = 0;  // r10e: when it went away (applet after a grace period)
  // rumble
  bool rumble_last = false;
  unsigned rumble_refresh = 0;
  // applet hotkey
  int chord_frames = 0;
  int style = -1;
};

std::once_flag g_init_once;
Player g_players[kPlayers];
bool g_motion_enabled = true;
bool g_motion_invert = false;
bool g_joy_rotate = false;
// r10e: on the user's console (8-nov) the system reported the horizontal hold
// but single Joy-Con sticks still came unrotated, so the stick is now always
// rotated by hand (NWII_NX_JOY_ROTATE=0 turns it off, =auto restores the check).
// r10: NWII_NX_JOY_ROTATE=auto: rotate a single Joy-Con's stick by
// hand only while the system does not report the horizontal hold (on the real
// console the stick came unrotated: something, likely SDL's joystick init,
// leaves the hold vertical). Checked again every ~2 s (it is an IPC call).
int g_joy_rotate_mode = 1;  // -1 auto, 0 never, 1 always (r10e: default always, see below)
bool joy_rotate_now() {
  if (g_joy_rotate_mode >= 0) return g_joy_rotate_mode == 1;
  static uint64_t last = 0;
  static bool rotate = false;
  const uint64_t now = armGetSystemTick();
  if (last == 0 || now - last >= 2 * 19200000ull) {
    last = now;
    HidNpadJoyHoldType ht = HidNpadJoyHoldType_Horizontal;
    const bool horiz = R_FAILED(hidGetNpadJoyHoldType(&ht)) || ht == HidNpadJoyHoldType_Horizontal;
    if (rotate != !horiz)
      std::printf("[NX-INPUT] Joy-Con hold type is %s: %s single Joy-Con sticks by hand\n",
                  horiz ? "horizontal" : "vertical", horiz ? "not rotating" : "rotating");
    rotate = !horiz;
  }
  return rotate;
}
bool g_six_rotate = true;  // NWII_NX_SIX_ROTATE=0: the system already turns it
bool g_single_mode = false;
std::atomic<bool> g_applet_pending{false};
uint64_t g_last_applet_tick = 0;  // r10e: when the last controller applet closed
u64 g_start_tick = 0;

bool env_set(const char *name) {
  const char *v = std::getenv(name);
  return v && *v && *v != '0';
}

void restore_assignment() {
  // Leave the Joy-Cons as the system had them: dual (paired) per Npad.
  for (int p = 0; p < kPlayers; ++p) hidSetNpadJoyAssignmentModeDual(g_players[p].id);
}

void do_init() {
  const Config &cfg = Config::get();
  std::string mode = cfg.joycon_mode;
  if (const char *e = std::getenv("NWII_NX_JOYCON")) mode = e;
  g_single_mode = mode == "single";

  // SDL configured 8 players with every standard style; the game reads up
  // to four sideways remotes: all standard styles, single Joy-Cons held
  // horizontally.
  padConfigureInput(kPlayers, HidNpadStyleSet_NpadStandard);
  hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
  for (int p = 0; p < kPlayers; ++p) {
    Player &pl = g_players[p];
    pl.id = (HidNpadIdType)(HidNpadIdType_No1 + p);
    if (p == 0)
      padInitialize(&pl.pad, HidNpadIdType_No1, HidNpadIdType_Handheld);
    else
      padInitializeWithMask(&pl.pad, 1UL << pl.id);
    padUpdate(&pl.pad);
    if (g_single_mode)
      hidSetNpadJoyAssignmentModeSingleByDefault(pl.id);
    else if (p == 0 || mode == "pair")
      hidSetNpadJoyAssignmentModeDual(pl.id);

    for (int s = 0; s < S_COUNT; ++s) {
      if (s == S_HANDHELD && p != 0) continue;
      const HidNpadIdType id = s == S_HANDHELD ? HidNpadIdType_Handheld : pl.id;
      const int n_six = kStyleSixAxis[s];
      if (R_SUCCEEDED(hidGetSixAxisSensorHandles(pl.six[s], n_six, id, kStyleTag[s]))) {
        pl.six_ok[s] = true;
        for (int i = 0; i < n_six; ++i) hidStartSixAxisSensor(pl.six[s][i]);
      }
      pl.vib_ok[s] = R_SUCCEEDED(hidInitializeVibrationDevices(
          pl.vib[s], kStyleVibration[s], id, kStyleTag[s]));
    }
  }
  if (g_single_mode) std::atexit(restore_assignment);

  // env.txt: NWII_NX_NO_MOTION=1 turns motion tilt/shake off (buttons only);
  // NWII_NX_TILT_INVERT=1 flips the sign of the motion tilt only;
  // NWII_NX_JOY_ROTATE=1 rotates single Joy-Con sticks by hand.
  g_motion_enabled = std::getenv("NWII_NX_NO_MOTION") == nullptr;
  g_motion_invert = std::getenv("NWII_NX_TILT_INVERT") != nullptr;
  g_joy_rotate = env_set("NWII_NX_JOY_ROTATE");
  if (const char *e = std::getenv("NWII_NX_JOY_ROTATE"))
    g_joy_rotate_mode = std::strcmp(e, "auto") == 0 ? -1 : (*e && *e != '0') ? 1 : 0;
  if (const char *e = std::getenv("NWII_NX_SIX_ROTATE")) g_six_rotate = *e != '0';
  g_start_tick = armGetSystemTick();
  std::printf("[NX-INPUT] %d players, Joy-Con %s, hold horizontal\n", kPlayers,
              g_single_mode ? "single" : (mode == "pair" ? "pair" : "as paired"));
}

// The style the player is using right now (handheld wins for player 1).
int active_style(Player &pl, int p) {
  if (p == 0 && (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld))
    return S_HANDHELD;
  const u32 st = hidGetNpadStyleSet(pl.id);
  if (st & HidNpadStyleTag_NpadFullKey) return S_FULLKEY;
  if (st & HidNpadStyleTag_NpadJoyDual) return S_JOYDUAL;
  if (st & HidNpadStyleTag_NpadJoyLeft) return S_JOYLEFT;
  if (st & HidNpadStyleTag_NpadJoyRight) return S_JOYRIGHT;
  return -1;
}

// Reads the accelerometer once per poll: updates the steering roll (only from
// samples close to 1 g, so a shake does not yank the tilt) and the shake pulse.
bool read_motion(Player &pl, int style, float *tilt_out, bool *shake_out) {
  *shake_out = false;
  if (pl.shake_pulse > 0) {
    --pl.shake_pulse;
    *shake_out = true;
  }
  if (pl.shake_cooldown > 0) --pl.shake_cooldown;
  if (!g_motion_enabled || style < 0 || !pl.six_ok[style]) return false;

  // Joy-Con pair: the right Joy-Con (the one with the face buttons).
  const HidSixAxisSensorHandle h = pl.six[style][style == S_JOYDUAL ? 1 : 0];
  HidSixAxisSensorState st = {};
  if (hidGetSixAxisSensorStates(h, &st, 1) == 0) return false;
  float x = st.acceleration.x;
  const float y = st.acceleration.y, z = st.acceleration.z;
  const float g = std::sqrt(x * x + y * y + z * z);
  if (g_six_rotate && (style == S_JOYLEFT || style == S_JOYRIGHT)) {
    // HID reports a single Joy-Con's six-axis in the controller's own
    // (upright, as if attached) frame; the hold type only turns the stick.
    // Held sideways the long axis (+Y, towards the stick of the left Joy-Con
    // and the buttons of the right one) becomes the remote's left-right axis:
    // the left Joy-Con is turned a quarter counter-clockwise (+Y points left),
    // the right one clockwise (+Y points right).
    x = style == S_JOYLEFT ? -y : y;
  }
  if (g < 0.5f) return false;  // no gravity reading: emulator or no sensor

  if (g > kShakeThresholdG && pl.shake_cooldown == 0) {
    pl.shake_pulse = kShakePulseFrames;
    pl.shake_cooldown = kShakePulseFrames + kShakeCooldownFrames;
    *shake_out = true;
  }
  if (g > 0.7f && g < 1.3f) {
    // Roll from the side (X) axis only, so it works with the controller held
    // flat or upright: turning right lowers the right end, which swings the
    // measured "up" towards -X (X already turned for single Joy-Cons above).
    // NWII_NX_TILT_INVERT=1 flips it if a controller reports the opposite
    // sign.
    float t = std::asin(std::fmax(-1.0f, std::fmin(1.0f, -x / g)));
    if (g_motion_invert) t = -t;
    const float mag = std::fabs(t) - kTiltDeadZone;
    pl.last_tilt = mag > 0.0f ? std::copysign(mag, t) : 0.0f;
  }
  *tilt_out = pl.last_tilt;
  return true;
}

std::atomic<bool> g_applet_from_add_screen{false};
std::atomic<bool> g_applet_running{false};
std::atomic<uint64_t> g_applet_due_tick{0};  // r10h: not before this tick
void queue_applet(const char *why) {
  // r10g: one applet at a time: nothing new while one is pending/open or for 3 s
  // after the last one (the system re-assigns Joy-Cons around an applet, which
  // looked like a lost controller and queued a second applet).
  if (g_applet_running.load() ||
      armTicksToNs(armGetSystemTick() - g_last_applet_tick) < 3'000'000'000ull)
    return;
  if (std::strcmp(why, "add-a-remote screen") == 0) {
    g_applet_from_add_screen.store(true);
    // r10h: let the game's own "add a remote" screen open first (its intro
    // animation), then show the applet on top of it.
    g_applet_due_tick.store(armGetSystemTick() + 19200000ull / 4);  // r10i: 0.25 s (0.8 s + the applet launch felt slow)
  } else {
    g_applet_due_tick.store(0);
  }
  if (!g_applet_pending.exchange(true))
    std::printf("[NX-INPUT] controller applet requested (%s)\n", why);
}

}  // namespace

void input_init() { std::call_once(g_init_once, do_init); }

// r10e: after the controller applet that the game's "add a remote" screen
// opened, a short + press on player 1 closes that screen (on the Wii the
// players press + when done). Counts polls of player 1: r10g: ~0.5 s idle so the
// game's screen is up and accepting input, then 8 polls pressed.
std::atomic<int> g_inject_plus{0};
static Pad poll_pad_raw(int player);
Pad poll_pad(int player) {
  Pad p = poll_pad_raw(player);
  if (player == 0) {
    const int n = g_inject_plus.load(std::memory_order_relaxed);
    if (n > 0) {
      g_inject_plus.store(n - 1, std::memory_order_relaxed);
      if (n == 8) std::printf("[NX-INPUT] closing the game's add-a-remote screen (+)\n");
      // r10k: once that + is out the screen is closed, so a new "add a
      // remote" request is the player opening it again: allow it ~0.5 s later
      // instead of after the 3 s applet cool-down.
      if (n == 1) g_last_applet_tick = armGetSystemTick() - 19200000ull * 5 / 2;
      if (p.connected && n <= 8) p.plus = true;
    }
  }
  return p;
}

static Pad poll_pad_raw(int player) {
  input_init();
  Pad p;
  if (player < 0 || player >= kPlayers) return p;
  Player &pl = g_players[player];
  padUpdate(&pl.pad);
  p.connected = padIsConnected(&pl.pad);
  const int style = active_style(pl, player);
  pl.style = p.connected ? style : -1;
  const uint64_t now_tick = armGetSystemTick();
  if (pl.was_connected && !p.connected && Config::get().nx_controller_applet &&
      armTicksToNs(now_tick - g_start_tick) > 5'000'000'000ull) {
    // A controller in use went away (battery, detached Joy-Con): let the
    // players reconnect, as Switch games do -- but only if it is still gone
    // after a grace period (r10e: attaching/detaching Joy-Cons drops the pads
    // for a moment while the system re-assigns them; opening the applet right
    // away stacked several applets and left the pair split).
    std::printf("[NX-INPUT] player %d lost its controller\n", player + 1);
    pl.lost_tick = now_tick;
  }
  if (p.connected) pl.lost_tick = 0;
  if (pl.lost_tick && armTicksToNs(now_tick - pl.lost_tick) > 1'500'000'000ull) {
    pl.lost_tick = 0;
    // r13: Joy-Cons put back on the console (handheld connected) = the player chose
    // handheld play: no applet for whichever player lost them (before, the applet
    // came up and offered the pair as detached Joy-Cons).
    const bool handheld_now =
        (hidGetNpadStyleSet(HidNpadIdType_Handheld) & HidNpadStyleTag_NpadHandheld) != 0;
    if (!handheld_now && armTicksToNs(now_tick - g_last_applet_tick) > 3'000'000'000ull)
      queue_applet("controller lost");
  }
  pl.was_connected = p.connected;
  if (!p.connected) {
    pl.chord_frames = 0;
    return p;
  }
  const u64 k = padGetButtons(&pl.pad);
  bool chord = false;

  if (style == S_JOYLEFT || style == S_JOYRIGHT) {
    // Single Joy-Con held sideways. The stick is the d-pad (the system turns
    // it for the horizontal hold; NWII_NX_JOY_ROTATE=1 does it by hand).
    HidAnalogStickState s0 = padGetStickPos(&pl.pad, 0);
    HidAnalogStickState s1 = padGetStickPos(&pl.pad, 1);
    const HidAnalogStickState s =
        (std::abs(s1.x) + std::abs(s1.y) > std::abs(s0.x) + std::abs(s0.y)) ? s1 : s0;
    float sx = (float)s.x / (float)JOYSTICK_MAX, sy = (float)s.y / (float)JOYSTICK_MAX;
    if (joy_rotate_now()) {
      // Left Joy-Con turned a quarter counter-clockwise, right one clockwise.
      const float tx = sx;
      if (style == S_JOYLEFT) { sx = -sy; sy = tx; }
      else { sx = sy; sy = -tx; }
    }
    p.left = sx < -kStickDir;
    p.right = sx > kStickDir;
    p.up = sy > kStickDir;
    p.down = sy < -kStickDir;
    if (style == S_JOYLEFT) {
      // Held sideways the arrows sit where the face buttons are:
      // Left = bottom, Down = right (-> 2), Up = left, Right = top (-> 1).
      p.two = k & (HidNpadButton_Left | HidNpadButton_Down);
      p.one = k & (HidNpadButton_Up | HidNpadButton_Right);
      p.a = k & HidNpadButton_LeftSL;
      p.shake = k & HidNpadButton_LeftSR;
      p.plus = k & HidNpadButton_Minus;
      p.minus = k & HidNpadButton_StickL;
      chord = (k & HidNpadButton_LeftSL) && (k & HidNpadButton_LeftSR) &&
              (k & HidNpadButton_StickL);
    } else {
      // Right Joy-Con sideways: A = bottom, X = right (-> 2),
      // B = left, Y = top (-> 1).
      p.two = k & (HidNpadButton_A | HidNpadButton_X);
      p.one = k & (HidNpadButton_B | HidNpadButton_Y);
      p.a = k & HidNpadButton_RightSL;
      p.shake = k & HidNpadButton_RightSR;
      p.plus = k & HidNpadButton_Plus;
      p.minus = k & HidNpadButton_StickR;
      chord = (k & HidNpadButton_RightSL) && (k & HidNpadButton_RightSR) &&
              (k & HidNpadButton_StickR);
    }
    if (chord) p.minus = p.a = p.shake = false;
  } else {
    // Pro Controller, handheld or a Joy-Con pair (same as player 1 before).
    p.left = k & (HidNpadButton_Left | HidNpadButton_StickLLeft);
    p.right = k & (HidNpadButton_Right | HidNpadButton_StickLRight);
    p.up = k & (HidNpadButton_Up | HidNpadButton_StickLUp);
    p.down = k & (HidNpadButton_Down | HidNpadButton_StickLDown);
    p.two = k & (HidNpadButton_A | HidNpadButton_B);
    p.one = k & (HidNpadButton_X | HidNpadButton_Y);
    p.a = k & HidNpadButton_L;
    p.b = k & HidNpadButton_ZL;
    p.plus = k & HidNpadButton_Plus;
    p.minus = k & HidNpadButton_Minus;
    chord = (k & HidNpadButton_StickL) && (k & HidNpadButton_StickR);
    p.home = (k & HidNpadButton_StickR) && !(k & HidNpadButton_StickL);
    p.shake = k & (HidNpadButton_R | HidNpadButton_ZR);

    const HidAnalogStickState rs = padGetStickPos(&pl.pad, 1);
    const float rx = (float)rs.x / (float)JOYSTICK_MAX;
    if (std::fabs(rx) > kStickDeadZone)
      p.stick_tilt = std::fmax(-1.0f, std::fmin(1.0f, rx));
  }

  if (chord) {
    if (++pl.chord_frames == kAppletHoldFrames) queue_applet("L3+R3 / SL+SR+stick");
  } else {
    pl.chord_frames = 0;
  }

  bool motion_shake = false;
  p.has_motion = read_motion(pl, style, &p.motion_tilt, &motion_shake);
  p.shake |= motion_shake;
  return p;
}

std::string describe(int player) {
  if (player < 0 || player >= kPlayers) return {};
  const int s = g_players[player].style;
  return s >= 0 ? kStyleName[s] : std::string();
}

void rumble(int player, bool on) {
  input_init();
  if (player < 0 || player >= kPlayers) return;
  Player &pl = g_players[player];
  // The HLE asks every frame while the motor runs; HD rumble keeps playing
  // the last value, so only resend on a change or twice a second.
  if (on == pl.rumble_last && (!on || (++pl.rumble_refresh % 30) != 0)) return;
  pl.rumble_last = on;

  const int s = active_style(pl, player);
  if (s < 0 || !pl.vib_ok[s]) return;
  HidVibrationValue v = {};
  v.freq_low = 160.0f;
  v.freq_high = 320.0f;
  if (on) {
    v.amp_low = 0.6f;
    v.amp_high = 0.6f;
  }
  const HidVibrationValue values[2] = {v, v};
  hidSendVibrationValues(pl.vib[s], values, kStyleVibration[s]);
}

void rumble(bool on) {
  for (int p = 0; p < kPlayers; ++p) rumble(p, on);
}

void game_wants_remotes() {
  // r10g: while our applet is up the game stalls (it waits for the render
  // thread), so right after it closes the game's every-frame sync request looks
  // like a new one: ignore requests for 3 s after an applet (the double applet
  // seen on the console).
  if (armTicksToNs(armGetSystemTick() - g_last_applet_tick) < 3'000'000'000ull) return;
  if (Config::get().nx_controller_applet) queue_applet("add-a-remote screen");
}

void run_pending_applet() {
  if (!g_applet_pending.load()) return;
  if (armGetSystemTick() < g_applet_due_tick.load()) return;  // r10h: still waiting
  if (!g_applet_pending.exchange(false)) return;
  g_applet_running.store(true);
  input_init();
  // r10: SDL's joystick init registers 8 controllers; the game takes at most
  // 4 sideways remotes, so the applet (and the system) get exactly that.
  padConfigureInput(kPlayers, HidNpadStyleSet_NpadStandard);
  hidSetNpadJoyHoldType(HidNpadJoyHoldType_Horizontal);
  HidLaControllerSupportArg arg;
  hidLaCreateControllerSupportArg(&arg);
  arg.hdr.player_count_min = 1;
  arg.hdr.player_count_max = (s8)std::max(1, std::min(Config::get().input_players, kPlayers));
  arg.hdr.enable_take_over_connection = 1;
  arg.hdr.enable_left_justify = 1;
  arg.hdr.enable_permit_joy_dual = 1;
  arg.hdr.enable_single_mode = 0;
  // Player boxes in the characters' colours: Mario, Luigi, Yellow / Blue Toad.
  static const HidLaControllerSupportArgColor kColors[4] = {
      {230, 30, 30, 255}, {40, 180, 40, 255}, {240, 200, 0, 255}, {40, 110, 230, 255}};
  static const char *const kNames[4] = {"Mario", "Luigi", "Toad", "Toad"};
  arg.hdr.enable_identification_color = 1;
  arg.enable_explain_text = 1;
  for (int i = 0; i < kPlayers; ++i) {
    arg.identification_color[i] = kColors[i];
    hidLaSetExplainText(&arg, kNames[i], (HidNpadIdType)(HidNpadIdType_No1 + i));
  }
  HidLaControllerSupportResultInfo info = {};
  // NWII_NX_APPLET_FORSYSTEM=1: the system variant, which always shows the
  // UI even when the current controllers already fit.
  const Result rc = env_set("NWII_NX_APPLET_FORSYSTEM")
                        ? hidLaShowControllerSupportForSystem(&info, &arg, false)
                        : hidLaShowControllerSupport(&info, &arg);
  g_last_applet_tick = armGetSystemTick();
  g_applet_running.store(false);
  for (Player &pl2 : g_players) pl2.lost_tick = 0;  // r10g: losses before/while the applet ran are settled
  // r10k: also when the applet was cancelled (B: rc = LibAppletBadExit): the
  // game's screen still has to close.
  if (g_applet_from_add_screen.exchange(false)) g_inject_plus.store(38);
  std::printf("[NX-INPUT] controller applet: rc=0x%x players=%d\n", (unsigned)rc,
              (int)info.player_count);
  // Players may have switched controllers or styles: start the motion and
  // hotkey state over.
  for (Player &pl : g_players) {
    pl.chord_frames = 0;
    pl.shake_pulse = pl.shake_cooldown = 0;
    pl.last_tilt = 0.0f;
    pl.rumble_last = false;
  }
}

}  // namespace nwii::runtime::nx
#endif  // __SWITCH__
