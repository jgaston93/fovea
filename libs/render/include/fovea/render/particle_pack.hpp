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
// the phase function's g are mineral dust's, calculated by Mie theory from
// measured size distributions; the albedo is a chosen colour, to be tuned by
// eye against video (D-040).
struct ParticleLook {
    // Mass extinction coefficient: optical depth per kg/m^2 of particles
    // along a ray. A particle's extinction per metre is its bulk density
    // times this. 320: Saharan dust over its full size range (0.1 to 100
    // um) at 550 nm, 0.27 to 0.35 m^2/g, mean 0.32, calculated with a Mie
    // code (spheres) from size distributions measured from aircraft (Ryder et
    // al., Atmos. Chem. Phys. 18, 17225-17257, 2018,
    // doi:10.5194/acp-18-17225-2018, section 2.6 and table 6).
    float extinction_m2_per_kg = 320.0F;
    // The light a particle scatters, as a fraction of what falls on it, in
    // linear sRGB. Chosen: dust's single-scattering albedo is about 0.95 at
    // 550 nm (Ryder et al., above, calculated the same way), and a thick
    // cloud's tan
    // comes from many scatterings compounding its small spectral
    // differences, which the multiple-scattering approximation doesn't
    // model (particles.slang); so the colour is set here and tuned by eye.
    std::array<float, 3> albedo{0.62F, 0.52F, 0.40F};
    // The Cornette-Shanks phase function's g (the sky's Mie phase): 0
    // scatters evenly, toward 1 mostly forward (dust glows looking toward
    // the sun). Its g isn't the asymmetry parameter (the mean cosine); 0.68
    // gives a mean cosine of 0.74, computed numerically: the asymmetry
    // parameter Ryder et al. (above) give for Saharan dust at 550 nm.
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

// Self-shadowing by half-angle slicing (D-041): Green, "Volumetric Particle
// Shadows", NVIDIA, 2013, after Kniss et al.'s volume shading (GPU Gems,
// 2004). The particles are sorted along the vector halfway between the view
// and the light's direction of travel, and drawn in slices, each into the
// view's image and then into a light buffer of the optical depth toward
// the light, so each slice is shadowed by those nearer the light.

// Each packed particle's centre relative to the anchor, ECEF axes (what
// the sort and the light's bounds use). No allocation.
void particle_centres(std::span<const GpuParticle> particles, std::span<const GpuOrigin> origins,
                      std::span<Vec3> out) noexcept;

// The light's frame over the particles (mirrors LightSpace in
// particles.slang, std430): an orthographic view along the light's travel
// covering their bounding sphere, which the light buffer holds.
struct GpuLightSpace {
    std::array<float, 3> centre{};  // [m] the bounding sphere's centre from the anchor, ECEF
    float inv_radius = 0.0F;        // 1 / its radius [1/m]
    std::array<float, 3> u{};       // ECEF unit vectors across the light buffer
    float unused0 = 0.0F;
    std::array<float, 3> v{};
    float unused1 = 0.0F;
    std::array<float, 3> travel{};  // the light's direction of travel (away from the sun)
    float unused2 = 0.0F;
};
static_assert(sizeof(GpuLightSpace) == 64, "must match LightSpace in particles.slang");

// The light space over particles with these centres, for light travelling
// along `travel` (a unit vector). No allocation.
GpuLightSpace light_space(std::span<const Vec3> centres, std::span<const GpuParticle> particles,
                          const Vec3& travel) noexcept;

// The sort axis and order (Green 2013, "Half-Angle Slice Rendering"): with
// the view looking the way the light travels (their dot product
// positive), the half vector of the two, the slices drawn front to back
// from the eye ("under"); otherwise of the reversed view and the light,
// back to front from the eye ("over"). Front to back from the light
// either way.
struct HalfAngle {
    Vec3 axis{0.0F, 0.0F, 1.0F};
    bool front_to_back = true;  // from the eye
};
HalfAngle half_angle(const Vec3& view_forward, const Vec3& travel) noexcept;

// The particles' indices in ascending order of dot(axis, centre): a radix
// sort over `keys` and `scratch` (each at least as long as centres, two
// halves of their length each). Writes centres.size() indices into
// `order`. No allocation.
void sort_along(std::span<const Vec3> centres, const Vec3& axis, std::span<std::uint32_t> keys,
                std::span<std::uint32_t> scratch, std::span<std::uint32_t> order) noexcept;

}  // namespace fovea
