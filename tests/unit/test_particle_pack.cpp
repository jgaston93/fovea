// Packing a frame's particles for the GPU (D-040): moved to the frame's
// time, faded, the invisible left out, origins placed from the anchor.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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

namespace fovea {
namespace {

// Green 2013's rule: the view looking the way the light travels, their
// half vector and front to back; otherwise the reversed view's and back to
// front.
TEST(ParticleSlicing, TheHalfAngleFollowsTheLight) {
    const Vec3 travel(0.0F, 0.0F, -1.0F);                                // the sun straight above
    const HalfAngle down = half_angle(Vec3(1.0F, 0.0F, -1.0F), travel);  // looking down: with it
    EXPECT_TRUE(down.front_to_back);
    EXPECT_NEAR(glm::length(down.axis), 1.0F, 1e-6F);
    EXPECT_NEAR(glm::dot(down.axis, glm::normalize(Vec3(1.0F, 0.0F, -1.0F) +
                                                   Vec3(0.0F, 0.0F, -1.0F) * std::sqrt(2.0F))),
                1.0F, 1e-5F);
    const HalfAngle up = half_angle(Vec3(1.0F, 0.0F, 1.0F), travel);  // looking up: into it
    EXPECT_FALSE(up.front_to_back);
    // The reversed view and the light: (-1, 0, -1)/sqrt2 + (0, 0, -1), normalised.
    const Vec3 want = glm::normalize(-glm::normalize(Vec3(1.0F, 0.0F, 1.0F)) + travel);
    EXPECT_NEAR(glm::dot(up.axis, want), 1.0F, 1e-5F);
    // Straight into the light: along it.
    const HalfAngle into = half_angle(Vec3(0.0F, 0.0F, 1.0F), travel);
    EXPECT_FALSE(into.front_to_back);
    EXPECT_NEAR(glm::dot(into.axis, travel), 1.0F, 1e-5F);
}

TEST(ParticleSlicing, TheSortIsAscendingAndStable) {
    std::vector<Vec3> centres;
    std::uint32_t rng = 7;
    for (int i = 0; i < 5000; ++i) {
        rng = rng * 1664525U + 1013904223U;
        const float x = static_cast<float>(static_cast<int>(rng >> 8) % 2001 - 1000) * 0.25F;
        centres.emplace_back(x, -x * 0.5F, 3.0F);  // negatives, positives and ties
    }
    const Vec3 axis = glm::normalize(Vec3(1.0F, 0.2F, 0.0F));
    std::vector<std::uint32_t> keys(centres.size() * 2);
    std::vector<std::uint32_t> scratch(centres.size() * 2);
    std::vector<std::uint32_t> order(centres.size());
    sort_along(centres, axis, keys, scratch, order);
    std::vector<std::uint32_t> want(centres.size());
    for (std::uint32_t i = 0; i < want.size(); ++i) {
        want[i] = i;
    }
    std::stable_sort(want.begin(), want.end(), [&](std::uint32_t a, std::uint32_t b) {
        return glm::dot(axis, centres[a]) < glm::dot(axis, centres[b]);
    });
    EXPECT_EQ(order, want);
}

TEST(ParticleSlicing, TheLightSpaceHoldsEveryParticle) {
    std::vector<Vec3> centres{Vec3(10.0F, 0.0F, 0.0F), Vec3(-20.0F, 5.0F, 1.0F),
                              Vec3(3.0F, -8.0F, 12.0F)};
    std::vector<GpuParticle> particles(3);
    for (GpuParticle& p : particles) {
        p.radius = 4.0F;
    }
    const Vec3 travel = glm::normalize(Vec3(0.3F, -0.2F, -1.0F));
    const GpuLightSpace l = light_space(centres, particles, travel);
    const Vec3 c(l.centre[0], l.centre[1], l.centre[2]);
    const Vec3 u(l.u[0], l.u[1], l.u[2]);
    const Vec3 v(l.v[0], l.v[1], l.v[2]);
    const Vec3 w(l.travel[0], l.travel[1], l.travel[2]);
    EXPECT_NEAR(glm::dot(u, v), 0.0F, 1e-6F);
    EXPECT_NEAR(glm::dot(u, w), 0.0F, 1e-6F);
    EXPECT_NEAR(glm::dot(glm::cross(u, v), w), 1.0F, 1e-5F);
    for (std::size_t i = 0; i < centres.size(); ++i) {
        // The whole sphere within the light buffer: |uv| + r <= 1 in radii.
        const Vec3 d = (centres[i] - c) * l.inv_radius;
        const float r = particles[i].radius * l.inv_radius;
        EXPECT_LE(std::abs(glm::dot(d, u)) + r, 1.0F + 1e-5F);
        EXPECT_LE(std::abs(glm::dot(d, v)) + r, 1.0F + 1e-5F);
    }
}

}  // namespace
}  // namespace fovea

namespace fovea {
namespace {

// A lopsided channel's rays all lie to one side of its axis: the sort is
// made for its central ray (D-041). The matrix as projection.cpp builds one
// for the eye's frame (x forward, y right, z down): 30 to 86 degrees right.
TEST(ParticleSlicing, TheCentralRayOfALopsidedChannel) {
    const float l = std::tan(glm::radians(30.0F));
    const float r = std::tan(glm::radians(86.0F));
    const float t = std::tan(glm::radians(17.0F));
    const float b = std::tan(glm::radians(-17.0F));
    Mat4 m(0.0F);
    m[0][0] = -(r + l) / (r - l);
    m[1][0] = 2.0F / (r - l);
    m[0][1] = (t + b) / (t - b);
    m[2][1] = 2.0F / (t - b);
    m[3][2] = 0.5F;
    m[0][3] = 1.0F;
    const Vec3 want = glm::normalize(Vec3(1.0F, (r + l) / 2.0F, -(t + b) / 2.0F));
    const Vec3 got = view_centre_ray(m);
    EXPECT_NEAR(glm::dot(got, want), 1.0F, 1e-5F);
    // Not the axis (clip w's row, x forward): the middle is 63 degrees right of it.
    EXPECT_LT(glm::dot(got, Vec3(1.0F, 0.0F, 0.0F)), 0.5F);
    // A symmetric one's middle is its axis.
    Mat4 s = m;
    s[0][0] = 0.0F;
    s[1][0] = 1.0F;
    EXPECT_NEAR(glm::dot(view_centre_ray(s), Vec3(1.0F, 0.0F, 0.0F)), 1.0F, 1e-6F);
}

}  // namespace

// The erosion's gain (D-051): 1 without erosion, and rising with it, as the
// erosion takes more of the mass. A flat detail volume (every level mid
// grey) and the real one's shape both.
TEST(ParticleErosion, TheGainGivesTheMassBack) {
    std::array<std::uint32_t, 256> flat{};
    flat[128] = 1000;
    std::array<std::uint32_t, 256> spread{};
    for (std::size_t k = 0; k < spread.size(); ++k) {
        spread[k] = 10;
    }
    for (const auto& h : {flat, spread}) {
        EXPECT_EQ(erosion_gain(h, 0.6F, 0.0F), 1.0F);
        const float light = erosion_gain(h, 0.6F, 0.3F);
        const float strong = erosion_gain(h, 0.6F, 0.8F);
        EXPECT_GT(light, 1.0F);
        EXPECT_GT(strong, light);
        EXPECT_LT(strong, 20.0F);
    }
    // An empty histogram: nothing to weigh, no gain.
    EXPECT_EQ(erosion_gain(std::array<std::uint32_t, 256>{}, 0.6F, 0.6F), 1.0F);
    // With the field everywhere 1 (n = 1), nothing is eroded but the outer
    // quarter of s's ramp (4 s, saturated): a gain of a few percent.
    std::array<std::uint32_t, 256> white{};
    white[255] = 1;
    EXPECT_GT(erosion_gain(white, 0.6F, 0.8F), 1.0F);
    EXPECT_LT(erosion_gain(white, 0.6F, 0.8F), 1.1F);
}

// The volume's box (D-052): it covers the particles; its voxel is a power
// of two of metres; its corner is on a multiple of the voxel in the world;
// and the same particles seen from another eye (anchor) give the same box in
// the world (a box fitted afresh each frame moved, and the cloud flickered).
TEST(ParticleVolume, TheBoxCoversTheParticlesAndStaysPut) {
    const DVec3 anchor(500000.25, -5000000.5, 3600000.75);
    std::vector<Vec3> centres{Vec3(-10.0F, 0.0F, 0.0F), Vec3(10.0F, 4.0F, -2.0F)};
    std::vector<GpuParticle> ps(2);
    ps[0].radius = 2.0F;
    ps[1].radius = 1.0F;
    const GpuVolumeSpace v = volume_space(centres, ps, 128, 64, 256.0F, anchor);
    EXPECT_EQ(v.size, 128U);
    EXPECT_EQ(v.light_size, 64U);
    EXPECT_EQ(std::exp2(std::round(std::log2(v.voxel))), v.voxel);  // a power of two
    const float side = v.voxel * 128.0F;
    for (std::size_t i = 0; i < 2; ++i) {
        for (std::size_t a = 0; a < 3; ++a) {
            const auto c = static_cast<glm::length_t>(a);
            EXPECT_LE(v.corner[a], centres[i][c] - ps[i].radius);
            EXPECT_GE(v.corner[a] + side, centres[i][c] + ps[i].radius);
        }
    }
    for (std::size_t a = 0; a < 3; ++a) {
        const double world =
            anchor[static_cast<glm::length_t>(a)] + static_cast<double>(v.corner[a]);
        const double voxel = static_cast<double>(v.voxel);
        EXPECT_NEAR(world / voxel, std::round(world / voxel), 1e-3) << a;
    }
    // The eye 3.3 m away: the particles' offsets from it change, the box's
    // grid in the world doesn't.
    const DVec3 moved = anchor + DVec3(3.3, -1.7, 0.9);
    std::vector<Vec3> from_moved;
    for (const Vec3& c : centres) {
        from_moved.push_back(Vec3(DVec3(c) + anchor - moved));
    }
    const GpuVolumeSpace w = volume_space(from_moved, ps, 128, 64, 256.0F, moved);
    EXPECT_EQ(w.voxel, v.voxel);
    for (std::size_t a = 0; a < 3; ++a) {
        const auto c = static_cast<glm::length_t>(a);
        // The same grid: the corners whole voxels apart (the box may step a
        // voxel as its bounds cross a grid line; its voxels don't move).
        const double apart = (moved[c] + static_cast<double>(w.corner[a]) -
                              (anchor[c] + static_cast<double>(v.corner[a]))) /
                             static_cast<double>(v.voxel);
        EXPECT_NEAR(apart, std::round(apart), 1e-3) << a;
    }
    // Capped: the voxel no larger than the cap allows; at least a metre's box.
    EXPECT_LE(volume_space(centres, ps, 128, 64, 10.0F, anchor).voxel * 128.0F, 10.0F);
    std::vector<Vec3> one{Vec3(0.0F)};
    std::vector<GpuParticle> tiny(1);
    tiny[0].radius = 0.01F;
    EXPECT_GE(volume_space(one, tiny, 64, 32, 256.0F, anchor).voxel * 64.0F, 1.0F);
}

// The volume's erosion gain (D-052): 1 without erosion; rising with it;
// and the eroded lone particle's mass given back (the render test,
// ParticlesVolume.ErosionKeepsTheMass, measures it).
TEST(ParticleErosion, TheVolumesGainRisesWithTheErosion) {
    std::array<std::uint32_t, 256> spread{};
    for (std::size_t k = 0; k < spread.size(); ++k) {
        spread[k] = 1;
    }
    EXPECT_EQ(volume_erosion_gain(spread, 0.6F, 0.0F), 1.0F);
    float last = 1.0F;
    for (const float e : {0.2F, 0.4F, 0.6F, 0.8F}) {
        const float g = volume_erosion_gain(spread, 0.6F, e);
        EXPECT_GT(g, last) << e;
        last = g;
    }
    EXPECT_LT(last, 4.0F);
}

}  // namespace fovea
