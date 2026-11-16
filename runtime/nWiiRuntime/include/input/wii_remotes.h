#pragma once
// Up to four virtual Wii Remotes (WPAD channels 0-3) for the WPAD/KPAD HLE
// (src/hle/wpad_hle.cpp), each driven by host devices:
//
//   PC (SDL2): player 1 = keyboard + first game controller by default; every
//   further controller that is plugged in becomes the next player (hotplug).
//   [input.player1..4] in the TOML picks the device, the buttons and the tilt
//   source per player (see docs in config.cpp / README "Controles").
//   Switch (libnx): player N = Npad No.N (player 1 also handheld); single
//   Joy-Cons held sideways, Pro Controllers and Joy-Con pairs all work, and the
//   system controller applet assigns them (see platform/switch/switch_input.h).
//
// Threading: remotes_update() runs on the host (main) thread once per host
// frame; it reads SDL/libnx and publishes a snapshot. remote_poll() and the
// other calls are safe from the guest CPU thread.
#include <cstdint>
#include <string>

namespace nwii::runtime::input {

constexpr int kMaxRemotes = 4;

// WPAD core button bits (sideways mapping is applied by the producer).
namespace wpad_btn {
constexpr uint32_t LEFT = 0x0001, RIGHT = 0x0002, DOWN = 0x0004, UP = 0x0008,
                   PLUS = 0x0010, TWO = 0x0100, ONE = 0x0200, B = 0x0400,
                   A = 0x0800, MINUS = 0x1000, HOME = 0x8000;
}

struct RemoteInput {
  bool connected = false;
  uint32_t buttons = 0;  // WPAD bits, already rotated for a sideways remote
  float tilt = 0.0f;     // steering-wheel roll, radians, + = turned right
  bool shake = false;    // a shake is requested this frame (held = repeat)
};

// Host thread, once per host frame (InputManager::update calls it).
void remotes_update();

// Guest CPU thread, once per game frame (KPAD read of channel 0): work that
// must pause the game, e.g. the Switch controller applet.
void remotes_guest_frame();

// Latest snapshot of one channel (0-3). Any thread.
RemoteInput remote_poll(int chan);

// Bit i set = channel i has a device. Any thread.
unsigned remote_connected_mask();

// The game's motor request for a channel (WPADControlMotor). Any thread; the
// host thread drives the device rumble.
void remote_set_motor(int chan, bool on);

// The game asked to accept new remotes (WPADSetAcceptConnection-like call
// from dRemoconMng, true while some enabled player slot has no remote).
void remote_set_accepting(bool accepting);

// The game shows its "add a remote" / sync screen (WPAD simple sync, called
// every frame while it is up). Switch: opens the controller applet once per
// visit of that screen.
void remote_request_sync();

// Debug/test: keep a channel connected with neutral input even without a
// device (scripted-input tests, NWII_WPAD_PLAYERS=n).
void remote_force_connected(int chan, bool on);

// Human-readable device list of a channel, for logs.
std::string remote_describe(int chan);

}  // namespace nwii::runtime::input
