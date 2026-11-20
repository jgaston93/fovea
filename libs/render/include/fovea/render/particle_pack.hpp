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

// How the particles look ([particles] in the config). The extinction and
// the phase function's g are measured mineral dust's; the albedo is a chosen
// colour, to be tuned by eye against video (D-040).
struct ParticleLook {
    // Mass extinction coefficient: optical depth per kg/m^2 of particles
    // along a ray. A particle's extinction per metre is its bulk density
    // times this. 320: Saharan dust over its full size range (0.1 to 100
    // um) at 550 nm, measured 0.27 to 0.35 m^2/g, mean 0.32 (Ryder et al.,
    // Atmos. Chem. Phys. 18, 17225-17257, 2018, doi:10.5194/acp-18-17225-2018).
    float extinction_m2_per_kg = 320.0F;
    // The light a particle scatters, as a fraction of what falls on it, in
    // linear sRGB. Chosen, not measured: dust's single-scattering albedo is
    // about 0.95 at 550 nm (Ryder et al., above), and a thick cloud's tan
    // comes from many scatterings compounding its small spectral
    // differences, which the multiple-scattering approximation doesn't
    // model (particles.slang); so the colour is set here and tuned by eye.
    std::array<float, 3> albedo{0.62F, 0.52F, 0.40F};
    // The Cornette-Shanks phase function's g (the sky's Mie phase): 0
    // scatters evenly, toward 1 mostly forward (dust glows looking toward
    // the sun). Its g isn't the asymmetry parameter (the mean cosine); 0.68
    // gives a mean cosine of 0.74, computed numerically, the asymmetry
    // Ryder et al. (above) measured for Saharan dust.
    float forward_scattering = 0.68F;
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
