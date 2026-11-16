#pragma once
// Switch-native input for the virtual Wii Remotes (input/wii_remotes.cpp).
// SDL's Switch backend exposes no Joy-Con sensors and no usable rumble, so
// buttons, tilt, shake and rumble are read/driven straight through libnx HID.
//
// Up to four players: player 1 = handheld or Npad No.1, player N = No.N.
// Every Npad style works, and the Wii Remote is always "held sideways":
//
//   Pro Controller / handheld / Joy-Con pair (dual):
//     A / B          -> 2 (jump, accept)      X / Y        -> 1 (run, back)
//     ZR / R         -> shake                 L            -> A (Wii)
//     ZL             -> B (Wii)               + / -        -> + / -
//     R3             -> HOME (the Switch HOME button belongs to the system;
//                       L3 + R3 held = controller applet, see below)
//     D-pad / L stick -> D-pad                R stick X    -> tilt
//   Single Joy-Con held sideways (horizontal hold, one per player):
//     stick -> D-pad; the two lower/right face buttons -> 2, the two
//     upper/left ones -> 1; SR -> shake, SL -> A; + or - -> +; stick click
//     -> - ; motion -> tilt.
//   Motion: steering-wheel roll of the controller -> tilt, a hard shake ->
//   shake (each controller's own six-axis sensor).
//
// Controller applet (who plays with what): hold L3 + R3 (or SL + SR + stick
// click on a single Joy-Con) for half a second; with [input]
// nx_controller_applet = true (default) it also opens by itself when the game
// shows its "Add a remote" screen (+ on the player-select screen) and when a
// controller in use is lost. It runs on the guest CPU thread, so the game is
// paused while it is shown.
//
// env.txt: NWII_NX_NO_MOTION=1 (buttons only), NWII_NX_TILT_INVERT=1 (flip
// motion tilt), NWII_NX_JOY_ROTATE=1 (rotate the stick of single Joy-Cons
// by hand if the system does not), NWII_NX_SIX_ROTATE=0 (do not turn the
// six-axis of single Joy-Cons to the sideways hold), NWII_NX_JOYCON=single|pair
// (= [input] joycon_mode).

#include <cstdint>
#include <string>

namespace nwii::runtime::nx {

constexpr int kPlayers = 4;

struct Pad {
  bool connected = false;
  bool left = false, right = false, up = false, down = false;
  bool two = false, one = false, a = false, b = false;
  bool plus = false, minus = false, home = false;
  bool shake = false;       // ZR / R / SR or a motion shake
  float stick_tilt = 0.0f;  // right stick X, [-1, 1], 0 inside the dead zone
  bool has_motion = false;  // motion_tilt is valid
  float motion_tilt = 0.0f; // steering-wheel roll in radians, + = turned right
};

void input_init();

// Polls one player (0-3). Host thread, once per frame.
Pad poll_pad(int player);

// "Joy-Con (L)", "Pro Controller", ... for logs; empty when nothing is there.
std::string describe(int player);

// HD rumble on a player's controller; the Wii Remote motor is on/off.
void rumble(int player, bool on);
// Every player at once (shutdown).
void rumble(bool on);

// The game opened its "add a remote" screen: queue the controller applet
// (if [input] nx_controller_applet).
void game_wants_remotes();

// Guest CPU thread, once per frame: shows the queued controller applet.
void run_pending_applet();

}  // namespace nwii::runtime::nx
