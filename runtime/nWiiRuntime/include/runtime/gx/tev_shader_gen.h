#pragma once
#include "runtime/gx_state.h"
#include <string>

namespace nwii::runtime::gx {

struct GeneratedShader {
    std::string vertex_source;
    std::string fragment_source;
};

GeneratedShader GenerateTEVShader(const GXState& state, uint8_t prim_type);

// R11 GPU XF: the vertex shader that pulls raw vertices and transforms them on
// the GPU (variant 0: `precise`, 1: products hidden from fusion; check: extra
// transform-feedback outputs xP xN xT01 xT23 xC), and its self-test shader.
std::string GenerateVertexShaderXF(int variant, bool check, int base_mode = 0);
std::string GenerateXfSelfTestShader(int variant);

} 
