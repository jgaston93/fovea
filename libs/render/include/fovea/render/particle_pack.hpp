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
    // Detail in the particles (D-043): their density varied by one field of
    // cellular noise in their origin's frame, shared by every particle, so
    // overlapping ones make one cloud of billows; its lookup swirled by curl
    // noise drifting in time; the mass kept on average. Chosen, to tune by
    // eye: how much it varies (0: none, uniform spheres as before), the
    // noise's feature size in metres, and the swirl's reach in features.
    float detail = 0.6F;
    float detail_size = 2.0F;  // [m]
    float swirl = 0.5F;
    // How far the same noise eats into each particle's outer shell (D-051):
    // the field subtracted there, so its outline follows the noise rather
    // than a sphere's. 0: not at all. Needs detail above 0.
    float erosion = 0.0F;
    // A particle's fade in and out (as the adapter's).
    std::chrono::duration<float> fade{0.25F};
};

// What the particles cost against how they look ([particles], D-042), fixed
// when the renderer is made.
struct ParticleQuality {
    bool self_shadowing = true;        // half-angle slicing (D-041); off: one slice, no light pass
    std::uint32_t slices = 32;         // the slices each view's particles are drawn in
    std::uint32_t light_buffer = 512;  // its texels a side
    bool half_resolution = false;      // drawn at half each view's size, composited up
    // Drawn as a volume instead (D-052): one box over the particles, its
    // voxels a side, its sun volume's, and its largest side (metres).
    bool volume = false;
    std::uint32_t volume_size = 128;
    std::uint32_t volume_light_size = 64;
    float volume_max_m = 256.0F;
    // Each particle spread over this many times its radius as it's added to
    // the volume, its density over the cube of it (its mass kept): sparse
    // particles merge into one cloud, as a fluid's particles are smoothed
    // onto a grid (D-052). 1: as sent.
    float volume_spread = 1.0F;
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
    float erosion = 0.0F;  // the look's (D-051): fixed at start, carried here for want of push room
    std::array<float, 3> travel{};  // the light's direction of travel (away from the sun)
    float erosion_gain = 1.0F;  // the eroded density scaled back to the mass (D-051, erosion_gain)
};
static_assert(sizeof(GpuLightSpace) == 64, "must match LightSpace in particles.slang");

// The volume's box (D-052): mirrors VolumeSpace in particles.slang.
struct GpuVolumeSpace {
    std::array<float, 3> corner{};  // [m] its lowest corner from the anchor, ECEF axes
    float voxel = 1.0F;             // [m] a voxel's side
    std::uint32_t size = 1;         // voxels a side
    std::uint32_t light_size = 1;   // the sun volume's voxels a side
    float inv_voxel = 1.0F;
    std::uint32_t particles = 0;  // how many to inject
    float spread = 1.0F;          // each particle's radius times this, its density over its cube
    float inv_reference =
        1.0F;  // 1 / the particles' mean extinction once spread [m] (erosion's scale)
    std::array<float, 2> unused{};
};
static_assert(sizeof(GpuVolumeSpace) == 48, "must match VolumeSpace in particles.slang");

// The box over particles with these centres (from the anchor, ECEF axes)
// and radii: a cube of `size` voxels, each a power of two of metres, the
// smallest holding their bounds with a voxel to spare each side (at most
// `max_side` metres a side), its corner on a multiple of the voxel in ECEF
// (`anchor`, ECEF), so the grid stays put in the world. No allocation.
GpuVolumeSpace volume_space(std::span<const Vec3> centres, std::span<const GpuParticle> particles,
                            std::uint32_t size, std::uint32_t light_size, float max_side,
                            const DVec3& anchor, float spread = 1.0F) noexcept;

// What the erosion (D-051) takes from a particle's mass, as the factor that
// gives it back: the particle's density integrated over its volume as
// particles.slang has it (the base, softened; the detail's factor) without
// erosion, over the same with it. `histogram` counts the detail volume's
// values as the GPU stores them (R8: level k is k / 255). 1 at erosion 0;
// more as it rises. No allocation.
float erosion_gain(std::span<const std::uint32_t, 256> histogram, float detail,
                   float erosion) noexcept;

// The same for the volume (D-052), whose erosion judges how deep in the
// cloud a point is by its density over the particles' mean (s = 1 -
// exp(-2 sigma / mean)) and carves with the field at three times its size
// (independent of the detail's): a lone particle's density integrated
// without erosion over with it. No allocation.
float volume_erosion_gain(std::span<const std::uint32_t, 256> histogram, float detail,
                          float erosion) noexcept;

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
// A view's central ray (ECEF, unit): where its clip x and y are 0, the
// middle of its frustum. Not the view's axis (clip w's row) for an
// asymmetric frustum, whose rays all lie to one side of the axis: the sort
// is right for rays near the direction it's made for (D-041).
Vec3 view_centre_ray(const Mat4& ecef_to_clip) noexcept;

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
