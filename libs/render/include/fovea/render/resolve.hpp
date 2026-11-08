#pragma once

// The weighted resolve of a multisampled pixel (D-028), as shaders/resolve.slang
// does it on the GPU: the samples' mean in linear HDR, each weighted by
// 1 / (1 + its luminance), so one very bright sample can't swamp an edge
// (which would stay jagged once tone-mapped). No weight is zero, so neither
// is their sum; samples are clamped to 65504, the largest 16-bit float,
// first. Here for tests, and as the shader's reference.

#include <span>

#include "fovea/core/math.hpp"

namespace fovea {

// Rec. 709 luminance of a linear colour (negative channels count as 0).
float luminance(const Vec3& c);

Vec3 weighted_resolve(std::span<const Vec3> samples);

}  // namespace fovea
