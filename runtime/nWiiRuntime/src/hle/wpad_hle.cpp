// Wii Remote HLE at the WPAD/KPAD API level.
//
// Replaces the Bluetooth stack (WUD/BTE over /dev/usb/oh1) with four virtual
// remotes (channels 0-3), held sideways: buttons, tilt (steering-wheel roll),
// shake and rumble per channel. The host devices behind each channel (SDL
// keyboard / game controllers on PC, libnx HID on Switch) and their hotplug
// live in src/input/wii_remotes.cpp; a channel without a device reads as "no
// controller", so the game's connect / disconnect flows (player join, "add a
// remote", remote-lost pause) see controllers come and go. Hooked per game
// through [hle_hooks]; every handler is named NWII_* so it never collides with
// a generated wrapper that carries the SDK symbol name.
//
// KPADStatus layout follows the 2009 RVL SDK (see NSMBW-Decomp KPAD.h).
#include "runtime/cpu_context.h"
#include "input/wii_remotes.h"
#include <cmath>
#include <cstdlib>
#include <iostream>

using namespace nwii::runtime;
namespace in = nwii::runtime::input;

namespace {

constexpr int32_t WPAD_ERR_NONE = 0;
constexpr int32_t WPAD_ERR_NO_CONTROLLER = -1;
constexpr int32_t WPAD_STATE_SETUP = 3;
constexpr uint32_t WPAD_DEV_CORE = 0;
constexpr uint32_t WPAD_DEV_NOT_FOUND = 253;
constexpr uint8_t WPAD_FMT_CORE_ACC = 1;

constexpr float kTiltSmoothing = 0.3f;
constexpr int kShakeFrames = 8;
constexpr int kShakeRepeat = 15;      // frames between shakes while held
constexpr float kShakeG = 2.5f;

// Per-channel state of the virtual remote, advanced on each KPAD read.
struct Remote {
  uint32_t prev_hold = 0;
  float tilt = 0.0f;
  int shake_frames = 0;
  int shake_hold = 0;
  float prev_acc_value = 1.0f;
  bool motor = false;
  bool connected = false;
  uint32_t frame = 0;
};

Remote g_remote[in::kMaxRemotes];

bool env_flag(const char *name) {
  const char *v = std::getenv(name);
  return v && *v && *v != '0';
}

bool valid_chan(int32_t chan) { return chan >= 0 && chan < in::kMaxRemotes; }

void write_vec2(MMU &mmu, uint32_t addr, float x, float y) {
  mmu.write_f32(addr, x);
  mmu.write_f32(addr + 4, y);
}

// Clears one KPADStatus to "no controller": no buttons held (the held ones
// are released), no motion, dev_type NOT_FOUND. EGG reads this one sample
// when KPADRead reports WPAD_ERR_NO_CONTROLLER.
void fill_kpad_absent(MMU &mmu, uint32_t s, Remote &r) {
  mmu.write32(s + 0x00, 0);
  mmu.write32(s + 0x04, 0);
  mmu.write32(s + 0x08, r.prev_hold);
  r.prev_hold = 0;
  r.tilt = 0.0f;
  r.shake_frames = r.shake_hold = 0;
  r.prev_acc_value = 1.0f;
  for (uint32_t off = 0x0C; off < 0x5C; off += 4) mmu.write32(s + off, 0);
  mmu.write8(s + 0x5C, (uint8_t)WPAD_DEV_NOT_FOUND);
  mmu.write8(s + 0x5D, (uint8_t)(int8_t)WPAD_ERR_NO_CONTROLLER);
  mmu.write8(s + 0x5E, 0);
  mmu.write8(s + 0x5F, WPAD_FMT_CORE_ACC);
  for (uint32_t off = 0x60; off < 0x84; off += 4) mmu.write32(s + off, 0);
}

// Fills one KPADStatus for a connected channel and advances its remote a frame.
void fill_kpad_status(MMU &mmu, uint32_t s, Remote &r, const in::RemoteInput &inp,
                      int32_t chan) {
  ++r.frame;
  uint32_t buttons = inp.buttons;
  if (chan == 0 && env_flag("NWII_WPAD_AUTO")) {
    // Headless boot testing: tap 2 + A every 90 frames.
    if (r.frame % 90 < 4)
      buttons |= in::wpad_btn::TWO | in::wpad_btn::A;
  }
  float tilt_target = inp.tilt;
  if (env_flag("NWII_TILT_INVERT"))
    tilt_target = -tilt_target;

  uint32_t hold = buttons;
  mmu.write32(s + 0x00, hold);
  mmu.write32(s + 0x04, hold & ~r.prev_hold);
  mmu.write32(s + 0x08, ~hold & r.prev_hold);
  r.prev_hold = hold;

  r.tilt += (tilt_target - r.tilt) * kTiltSmoothing;

  if (inp.shake) {
    if (r.shake_hold % kShakeRepeat == 0)
      r.shake_frames = kShakeFrames;
    ++r.shake_hold;
  } else {
    r.shake_hold = 0;
  }

  // KPAD axes: x across the remote, y out of the button face, z along the
  // remote towards the expansion port. Flat and face-up reads (0, -1, 0).
  // Held sideways the IR end points left, so turning the wheel right raises
  // it, which KPAD reports as a positive vertical angle.
  float ax = 0.0f;
  float ay = -std::cos(r.tilt);
  float az = -std::sin(r.tilt);
  if (r.shake_frames > 0) {
    ay += (r.shake_frames & 1) ? kShakeG : -kShakeG;
    --r.shake_frames;
  }
  float acc_value = std::sqrt(ax * ax + ay * ay + az * az);
  mmu.write_f32(s + 0x0C, ax);
  mmu.write_f32(s + 0x10, ay);
  mmu.write_f32(s + 0x14, az);
  mmu.write_f32(s + 0x18, acc_value);
  mmu.write_f32(s + 0x1C, acc_value - r.prev_acc_value);
  r.prev_acc_value = acc_value;

  write_vec2(mmu, s + 0x20, 0.0f, 0.0f);  // pos
  write_vec2(mmu, s + 0x28, 0.0f, 0.0f);  // vec
  mmu.write_f32(s + 0x30, 0.0f);          // speed
  write_vec2(mmu, s + 0x34, 1.0f, 0.0f);  // horizon
  write_vec2(mmu, s + 0x3C, 0.0f, 0.0f);  // hori_vec
  mmu.write_f32(s + 0x44, 0.0f);          // hori_speed
  mmu.write_f32(s + 0x48, 0.0f);          // dist
  mmu.write_f32(s + 0x4C, 0.0f);          // dist_vec
  mmu.write_f32(s + 0x50, 0.0f);          // dist_speed

  // acc_vertical as KPAD computes it: normalise(|xy|, -z), gravity only,
  // which for this model is already unit length.
  write_vec2(mmu, s + 0x54, std::fabs(std::cos(r.tilt)), std::sin(r.tilt));

  mmu.write8(s + 0x5C, WPAD_DEV_CORE);
  mmu.write8(s + 0x5D, (uint8_t)WPAD_ERR_NONE);
  mmu.write8(s + 0x5E, 0);  // dpd_valid_fg: no pointer
  mmu.write8(s + 0x5F, WPAD_FMT_CORE_ACC);
  for (uint32_t off = 0x60; off < 0x84; off += 4)
    mmu.write32(s + off, 0);  // ex_status: no extension
}

void note_connection(int32_t chan, bool connected) {
  Remote &r = g_remote[chan];
  if (r.connected == connected) return;
  r.connected = connected;
  if (!connected) {
    r.motor = false;
    in::remote_set_motor(chan, false);
  }
  if (env_flag("NWII_WPAD_LOG"))
    std::cout << "[WPAD-HLE] channel " << chan
              << (connected ? " connected" : " disconnected") << std::endl;
}

int32_t kpad_read(CPUContext &ctx, uint32_t err_ptr) {
  int32_t chan = (int32_t)ctx.gpr[3];
  uint32_t bufs = ctx.gpr[4];
  uint32_t length = ctx.gpr[5];
  if (!valid_chan(chan)) {
    if (err_ptr) ctx.mmu.write32(err_ptr, (uint32_t)WPAD_ERR_NO_CONTROLLER);
    return 0;
  }
  if (chan == 0) in::remotes_guest_frame();
  const in::RemoteInput inp = in::remote_poll(chan);
  note_connection(chan, inp.connected);
  Remote &r = g_remote[chan];
  if (!inp.connected) {
    if (err_ptr) ctx.mmu.write32(err_ptr, (uint32_t)WPAD_ERR_NO_CONTROLLER);
    if (bufs && length != 0) fill_kpad_absent(ctx.mmu, bufs, r);
    return 0;
  }
  if (err_ptr) ctx.mmu.write32(err_ptr, (uint32_t)WPAD_ERR_NONE);
  if (!bufs || length == 0)
    return 0;
  fill_kpad_status(ctx.mmu, bufs, r, inp, chan);
  return 1;
}

void log_once(const char *what) {
  static bool done = false;
  if (!done) {
    std::cout << "[WPAD-HLE] " << what
              << ": virtual sideways remotes on channels 0-3 (player 1 always "
                 "connected, 2-4 when a controller is assigned)"
              << std::endl;
    done = true;
  }
}

void set_accepting(CPUContext &ctx) {
  in::remote_set_accepting(ctx.gpr[3] != 0);
  ctx.gpr[3] = 1;
}

}  // namespace

extern "C" {

void NWII_HLE_Nop(CPUContext &ctx) {}
void NWII_HLE_Return0(CPUContext &ctx) { ctx.gpr[3] = 0; }
void NWII_HLE_Return1(CPUContext &ctx) { ctx.gpr[3] = 1; }
void NWII_HLE_ReturnNeg1(CPUContext &ctx) { ctx.gpr[3] = (uint32_t)-1; }

// KPADInitEx / WPADInit: never start the Bluetooth stack.
void NWII_KPADInitEx(CPUContext &ctx) { log_once("KPADInitEx"); }

void NWII_KPADRead(CPUContext &ctx) { ctx.gpr[3] = kpad_read(ctx, 0); }
void NWII_KPADReadEx(CPUContext &ctx) {
  ctx.gpr[3] = kpad_read(ctx, ctx.gpr[6]);
}

// WPADGetStatus(void) (SMNE01 0x801dfb30 -> WUDGetStatus): the Bluetooth
// stack state, 3 = ready. The "Add a remote" screen waits for 3 after
// WPADStopSimpleSync before it closes (Accept / +).
// (Older nsmbw.toml files bound 0x801dfbd0 to this name; that one is the
// accept-connection switch below, for which the old constant 3 = "accept" is
// harmless.)
void NWII_WPADGetStatus(CPUContext &ctx) { ctx.gpr[3] = WPAD_STATE_SETUP; }

// BOOL WPADSetAcceptConnection-like switch (see above), proper name.
void NWII_WPADSetAcceptConnection(CPUContext &ctx) { set_accepting(ctx); }

// BOOL WPADStartSimpleSync(void) (SMNE01 0x801dfac0 -> WUD): the game's "Add
// a remote" screen ("press 1 and 2 at the same time", + on the player-select
// screen). Report the sync as started: the screen only runs (and only closes
// with + / Accept) after that; when it fails the game retries every 100 ms
// and the screen hangs, which is what the old Return0 binding did. Remotes
// come from host controllers here; the Switch shows its controller applet.
void NWII_WPADStartSimpleSync(CPUContext &ctx) {
  in::remote_request_sync();
  ctx.gpr[3] = 1;
}

// BOOL WPADStopSimpleSync(void) (0x801dfad0): stopped (else the game retries
// it from an alarm every 100 ms).
void NWII_WPADStopSimpleSync(CPUContext &ctx) { ctx.gpr[3] = 1; }

// s32 WPADProbe(s32 chan, u32 *type)
void NWII_WPADProbe(CPUContext &ctx) {
  int32_t chan = (int32_t)ctx.gpr[3];
  uint32_t type_ptr = ctx.gpr[4];
  const bool connected = valid_chan(chan) && in::remote_poll(chan).connected;
  if (!connected) {
    if (type_ptr) ctx.mmu.write32(type_ptr, WPAD_DEV_NOT_FOUND);
    ctx.gpr[3] = (uint32_t)WPAD_ERR_NO_CONTROLLER;
    return;
  }
  if (type_ptr) ctx.mmu.write32(type_ptr, WPAD_DEV_CORE);
  ctx.gpr[3] = WPAD_ERR_NONE;
}

// void WPADControlMotor(s32 chan, u32 command)
void NWII_WPADControlMotor(CPUContext &ctx) {
  int32_t chan = (int32_t)ctx.gpr[3];
  if (!valid_chan(chan)) return;
  bool on = ctx.gpr[4] != 0;
  if (on != g_remote[chan].motor) {
    g_remote[chan].motor = on;
    in::remote_set_motor(chan, on);
  }
}

// void WPADDisconnect(s32 chan): the game drops a remote it does not want
// (e.g. more remotes than players). A virtual remote cannot be switched off,
// so it stays; the game keeps ignoring it, as it does with a remote that
// reconnects right away.
void NWII_WPADDisconnect(CPUContext &ctx) {
  int32_t chan = (int32_t)ctx.gpr[3];
  static bool logged[in::kMaxRemotes] = {};
  if (valid_chan(chan) && !logged[chan] && env_flag("NWII_WPAD_LOG")) {
    logged[chan] = true;
    std::cout << "[WPAD-HLE] game asked to disconnect remote " << (chan + 1)
              << std::endl;
  }
}

void NWII_WPADGetRadioSensitivity(CPUContext &ctx) { ctx.gpr[3] = 100; }

}  // extern "C"
