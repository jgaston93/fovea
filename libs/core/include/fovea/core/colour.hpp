#pragma once

// sRGB 8-bit values and linear light (IEC 61966-2-1), for averaging colour
// as light does: terrain imagery's mips (the compiler) and model textures'
// (libs/models). The renderer converts on the GPU.

#include <cstdint>

namespace fovea {

float srgb_to_linear(std::uint8_t v);  // from a table
std::uint8_t linear_to_srgb(float v);  // rounded, clamped to 0-1

}  // namespace fovea
