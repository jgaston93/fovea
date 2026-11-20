#pragma once

// Particle volumes (D-039, D-040): what the render thread does with each
// frame's ParticleFrame before the GPU draws it. Each particle is moved on
// its velocity to the time the frame is seen, faded, and packed with its
// extinction for the dust pass (shaders/particles.slang); each origin is
// placed relative to one anchor, in double, so the GPU sees small offsets.
// No Vulkan here: the packing is tested without a GPU.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

#include "fovea/core/math.hpp"
#include "fovea/core/particles.hpp"
#include "fovea/core/time.hpp"

namespace fovea {

// How the particles look ([particles] in the config). The defaults are a
// starting point for dust, to be tuned by eye against video, not measured
// values (D-040).
struct ParticleLook {
    // Mass extinction coefficient: optical depth per kg/m^2 of particles
    // along a ray. A particle's extinction per metre is its bulk density
    // times this.
    float extinction_m2_per_kg = 500.0F;
    // The light a particle scatters, as a fraction of what falls on it, in
    // linear sRGB: its single-scattering albedo times its colour.
    std::array<float, 3> albedo{0.62F, 0.52F, 0.40F};
    // Henyey-Greenstein asymmetry: 0 scatters evenly, toward 1 mostly
    // forward (dust glows looking toward the sun).
    float forward_scattering = 0.6F;
    // A particle's fade in and out (as the adapter's).
    std::chrono::duration<float> fade{0.25F};
};

// Mirrors Particle in shaders/particles.slang (std430).
struct GpuParticle {
    std::array<float, 3> position{};  // [m] east, north, up from its origin, at the frame's time
    float radius = 0.0F;              // [m]
    float extinction = 0.0F;          // [1/m] bulk density x the coefficient x its opacity
    std::uint32_t origin = 0;         // index into the origins
    std::array<float, 2> unused{};
};
static_assert(sizeof(GpuParticle) == 32, "must match Particle in particles.slang");

// Mirrors Origin in shaders/particles.slang (std430).
struct GpuOrigin {
    std::array<float, 3> offset{};  // [m] the origin from the anchor, ECEF axes
    float unused0 = 0.0F;
    std::array<float, 3> east{};  // ECEF unit vectors
    float unused1 = 0.0F;
    std::array<float, 3> north{};
    float unused2 = 0.0F;
    std::array<float, 3> up{};
    float unused3 = 0.0F;
};
static_assert(sizeof(GpuOrigin) == 64, "must match Origin in particles.slang");

// Packs `frame`'s particles as seen at `at` into `out`, and its origins
// relative to `anchor` (ECEF) into `origins`: those fully faded, of no
// size or no density, are left out. Returns how many particles were
// packed (at most out.size()). No allocation.
std::size_t pack_particles(const ParticleFrame& frame, LocalTime at, const ParticleLook& look,
                           const DVec3& anchor, std::span<GpuParticle> out,
                           std::span<GpuOrigin> origins) noexcept;

}  // namespace fovea
