#pragma once

// The noise volumes particles' detail is made from (D-043), built once at
// start on the CPU, tiling in all three directions:
// - Perlin's improved noise, after his reference implementation
//   (https://mrl.cs.nyu.edu/~perlin/noise/, "Improved Noise reference
//   implementation", 2002, for his SIGGRAPH 2002 paper): its fade, its
//   gradients from the hash's low 4 bits, its corner hashes and lerps. Two
//   changes: the lattice wraps at a period, so the volume tiles, and the
//   permutation is this file's own fixed shuffle (any permutation serves).
// - Cellular noise: the distance to the nearest of one random point per
//   cell, the 26 neighbouring cells searched, wrapping. After Worley's ("A
//   Cellular Texture Basis Function", SIGGRAPH 1996; his paper couldn't be
//   fetched and wasn't read: one point per cell may simplify his
//   scattering). Inverted, it is what Schneider and Vos found makes
//   "tightly packed billow shapes", layered at increasing frequencies in a
//   32^3 detail texture ("The Real-time Volumetric Cloudscapes of Horizon:
//   Zero Dawn", SIGGRAPH 2015 Advances in Real-Time Rendering, slides 31
//   and 34; read).
// - Curl noise (Bridson, Hourihan and Nordenstam, "Curl-Noise for
//   Procedural Fluid Flow", SIGGRAPH 2007): the curl of a vector potential
//   of three offset noises (their equation 1, by finite differences), so
//   divergence-free: a swirl with no sinks.
// No Vulkan here: the volumes are tested without a GPU.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "fovea/core/math.hpp"

namespace fovea {

// Perlin's improved noise at `p`, its lattice wrapping every `period` cells
// (`period` at most 256). About -1 to 1. Each `variant` (0 to 255) is
// another noise: its first hash lookup offset, as another permutation.
float periodic_perlin(const Vec3& p, int period, int variant = 0) noexcept;

// Inverted cellular noise at `p`: 1 at a cell's point, falling with the
// distance to the nearest (one point per cell, `period` cells a side,
// wrapping). 0 to 1.
float periodic_cellular(const Vec3& p, int period) noexcept;

// A size^3 volume, x fastest, of fractal inverted cellular noise (octaves
// of halving amplitude and doubling frequency, `cells` cells a side at
// the first), stretched to span 0 to 1 exactly, tiling.
std::vector<float> detail_volume(int size, int cells, int octaves);

// A size^3 volume, x fastest, of curl noise: the curl of three Perlin
// noises (three variants: Bridson's "large offsets" don't separate a noise
// that wraps, which would give the three components one function), `cells`
// lattice cells a side, octaves summed with amplitude halving, tiling.
// Scaled so its largest length is 1.
std::vector<Vec3> curl_volume(int size, int cells, int octaves);

}  // namespace fovea
