#pragma once
// r16res: fractional internal resolution. Runtime-only header (the generated
// game code never includes it), so it can change without a full rebuild; the
// Config struct (config.h) keeps its int fields untouched.
//
// [graphics] internal_resolution / internal_resolution_handheld and the
// NWII_INTERNAL_RES environment variable accept:
//   "auto"            PC: the presented frame fills the window height 1:1;
//                     Switch: "1080p" docked, "720p" handheld
//   1 .. 4            integer scale (1 = the Wii's 640x528 EFB)
//   "1.3636", "1.5x"  any scale from 1 to 4
//   "720p", "1080p"   the scale at which the presented XFB (456 lines in
//                     SMNE01) is exactly that many lines: 1:1 on the screen
//                     (720p = 720/456 = 1.579, 1080p = 2.368)
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace nwii::runtime::res {

struct Spec {
  enum Kind : unsigned char { Unset, Auto, Scale, Lines } kind = Unset;
  float value = 0.0f;  // Scale: the scale; Lines: the target lines
};

// Parsed by Config::load (config.cpp): [0] internal_resolution,
// [1] internal_resolution_handheld (Unset = same as [0]).
extern Spec g_cfg[2];

inline bool Parse(const char *s, Spec &out) {
  if (!s) return false;
  while (*s == ' ') ++s;
  if (!*s) return false;
  if (!std::strcmp(s, "auto")) { out = {Spec::Auto, 0.0f}; return true; }
  char *end = nullptr;
  const double v = std::strtod(s, &end);
  if (end == s || !(v > 0.0)) return false;
  if (*end == 'p' || *end == 'P') {
    if (end[1] || v < 120.0 || v > 4320.0) return false;
    out = {Spec::Lines, (float)v};
    return true;
  }
  if (*end == 'x' || *end == 'X') ++end;
  if (*end) return false;
  out = {Spec::Scale, (float)std::fmin(4.0, std::fmax(1.0, v))};
  return true;
}

inline std::string Describe(const Spec &s) {
  char b[48];
  switch (s.kind) {
    case Spec::Scale: std::snprintf(b, sizeof b, "%gx", s.value); return b;
    case Spec::Lines: std::snprintf(b, sizeof b, "%gp", s.value); return b;
    case Spec::Auto: return "auto";
    default: return "unset";
  }
}

// Scale for a spec: `lines` is the height the presented frame should have
// (auto), `xfb_lines` the guest height of the presented XFB. 1..4.
inline float Resolve(const Spec &s, float auto_lines, int xfb_lines) {
  if (xfb_lines <= 0) xfb_lines = 456;
  double v = 1.0;
  if (s.kind == Spec::Scale) v = s.value;
  else if (s.kind == Spec::Lines) v = s.value / xfb_lines;
  else v = auto_lines > 0.0f ? auto_lines / xfb_lines : 1.0;
  // A scale within 0.1 % of an integer is that integer (the integer paths are
  // the bit-exact ones); others keep 4 decimals so a window or a setting
  // gives the same scale every frame.
  const double r = std::round(v);
  if (std::fabs(v - r) < 0.001 * r) v = r;
  else v = std::round(v * 10000.0) / 10000.0;
  return (float)std::fmin(4.0, std::fmax(1.0, v));
}

}  // namespace nwii::runtime::res
