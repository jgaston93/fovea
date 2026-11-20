#include "fovea/render/particle_pack.hpp"

#include <algorithm>

namespace fovea {

namespace {

std::array<float, 3> floats3(const DVec3& v) noexcept {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

}  // namespace

std::size_t pack_particles(const ParticleFrame& frame, LocalTime at, const ParticleLook& look,
                           const DVec3& anchor, std::span<GpuParticle> out,
                           std::span<GpuOrigin> origins) noexcept {
    const std::size_t n_origins = std::min(frame.origins.size(), origins.size());
    for (std::size_t i = 0; i < n_origins; ++i) {
        const ParticleOrigin& o = frame.origins[i];
        GpuOrigin& g = origins[i];
        g.offset = floats3(o.ecef - anchor);
        g.east = floats3(o.enu_to_ecef[0]);
        g.north = floats3(o.enu_to_ecef[1]);
        g.up = floats3(o.enu_to_ecef[2]);
    }
    std::size_t n = 0;
    const std::size_t count = std::min(frame.count, frame.particles.size());
    for (std::size_t i = 0; i < count && n < out.size(); ++i) {
        const Particle& p = frame.particles[i];
        if (p.origin >= n_origins || p.radius <= 0.0F || p.bulk_density <= 0.0F) {
            continue;
        }
        const float opacity = particle_opacity_at(p, at, look.fade);
        if (opacity <= 0.0F) {
            continue;
        }
        const Vec3 pos = particle_position_at(p, at);
        GpuParticle& g = out[n++];
        g.position = {pos.x, pos.y, pos.z};
        g.radius = p.radius;
        g.extinction = p.bulk_density * look.extinction_m2_per_kg * opacity;
        g.origin = p.origin;
    }
    return n;
}

}  // namespace fovea
