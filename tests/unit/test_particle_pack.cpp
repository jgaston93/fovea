// Packing a frame's particles for the GPU (D-040): moved to the frame's
// time, faded, the invisible left out, origins placed from the anchor.

#include <array>
#include <chrono>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/particles.hpp"
#include "fovea/render/particle_pack.hpp"

namespace fovea {
namespace {

using namespace std::chrono_literals;

ParticleFrame frame(std::size_t n) {
    ParticleFrame f;
    f.particles.resize(n);
    ParticleOrigin o;
    o.ecef = DVec3(6378137.0 + 100.0, 2000.0, 3000.0);
    o.enu_to_ecef = DMat3(DVec3(0, 1, 0), DVec3(0, 0, 1), DVec3(1, 0, 0));
    f.origins.push_back(o);
    return f;
}

Particle particle(LocalTime t, float east, float v_east) {
    Particle p;
    p.position = Vec3(east, 2.0F, 3.0F);
    p.velocity = Vec3(v_east, 0.0F, 0.0F);
    p.radius = 1.5F;
    p.bulk_density = 0.002F;
    p.received = t;
    p.first_seen = t - 10s;
    p.fade_from = t + 10s;
    return p;
}

TEST(ParticlePack, MovesFadesAndPlacesThem) {
    const LocalTime t = LocalTime{} + 100s;
    ParticleFrame f = frame(4);
    f.particles[0] = particle(t, 10.0F, 4.0F);
    f.particles[1] = particle(t, 20.0F, 0.0F);
    f.particles[1].first_seen = t - 125ms;  // halfway faded in at t + 0 (fade 0.25 s)
    f.particles[2] = particle(t, 30.0F, 0.0F);
    f.particles[2].fade_from = t - 1s;  // long gone
    f.particles[3] = particle(t, 40.0F, 0.0F);
    f.particles[3].bulk_density = 0.0F;  // nothing to see
    f.count = 4;
    ParticleLook look;
    look.extinction_m2_per_kg = 100.0F;
    std::array<GpuParticle, 8> out{};
    std::array<GpuOrigin, kMaxParticleOrigins> origins{};
    const DVec3 anchor(6378137.0, 2000.0, 3000.0);
    const std::size_t n = pack_particles(f, t + 500ms, look, anchor, out, origins);
    ASSERT_EQ(n, 2U);
    EXPECT_FLOAT_EQ(out[0].position[0], 12.0F);  // 10 m + 4 m/s x 0.5 s
    EXPECT_FLOAT_EQ(out[0].radius, 1.5F);
    EXPECT_FLOAT_EQ(out[0].extinction, 0.2F);  // 0.002 kg/m^3 x 100 m^2/kg, fully in
    EXPECT_FLOAT_EQ(out[1].position[0], 20.0F);
    EXPECT_FLOAT_EQ(out[1].extinction, 0.2F);  // in by t + 0.5 s
    EXPECT_FLOAT_EQ(origins[0].offset[0], 100.0F);
    EXPECT_FLOAT_EQ(origins[0].east[1], 1.0F);
    EXPECT_FLOAT_EQ(origins[0].up[0], 1.0F);
    // Halfway faded in, at t: half the extinction.
    const std::size_t m = pack_particles(f, t, look, anchor, out, origins);
    ASSERT_EQ(m, 2U);
    EXPECT_NEAR(out[1].extinction, 0.1F, 1e-6F);
}

TEST(ParticlePack, StopsAtTheOutputsSizeAndSkipsUnknownOrigins) {
    const LocalTime t = LocalTime{} + 100s;
    ParticleFrame f = frame(5);
    for (std::size_t i = 0; i < 5; ++i) {
        f.particles[i] = particle(t, static_cast<float>(i), 0.0F);
    }
    f.particles[1].origin = 7;  // no such origin
    f.count = 5;
    std::array<GpuParticle, 3> out{};
    std::array<GpuOrigin, kMaxParticleOrigins> origins{};
    const std::size_t n = pack_particles(f, t, ParticleLook{}, DVec3(0.0), out, origins);
    ASSERT_EQ(n, 3U);
    EXPECT_FLOAT_EQ(out[0].position[0], 0.0F);
    EXPECT_FLOAT_EQ(out[1].position[0], 2.0F);
    EXPECT_FLOAT_EQ(out[2].position[0], 3.0F);
}

}  // namespace
}  // namespace fovea
