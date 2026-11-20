// The noise volumes particles' detail is made from (D-043): Perlin's
// improved noise and cellular noise wrap at their period; the curl volume
// is divergence-free (Bridson et al. 2007) and tiles.

#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/render/particle_noise.hpp"

namespace fovea {
namespace {

TEST(ParticleNoise, PerlinWrapsAtItsPeriodAndStaysInRange) {
    float lo = 1.0F;
    float hi = -1.0F;
    for (int i = 0; i < 2000; ++i) {
        const Vec3 p(0.37F * static_cast<float>(i % 17), 0.53F * static_cast<float>(i % 23),
                     0.71F * static_cast<float>(i % 29));
        const float v = periodic_perlin(p, 8);
        EXPECT_NEAR(v, periodic_perlin(p + Vec3(8.0F, 0.0F, 0.0F), 8), 1e-5F);
        EXPECT_NEAR(v, periodic_perlin(p + Vec3(0.0F, 8.0F, 8.0F), 8), 1e-5F);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    EXPECT_GE(lo, -1.5F);
    EXPECT_LE(hi, 1.5F);
    EXPECT_LT(lo, -0.2F);  // it varies
    EXPECT_GT(hi, 0.2F);
    // Zero at every lattice point, as gradient noise is.
    EXPECT_NEAR(periodic_perlin(Vec3(3.0F, 5.0F, 2.0F), 8), 0.0F, 1e-6F);
}

TEST(ParticleNoise, CellularWrapsAndIsOneAtItsPoints) {
    for (int i = 0; i < 500; ++i) {
        const Vec3 p(0.41F * static_cast<float>(i % 13), 0.29F * static_cast<float>(i % 19),
                     0.67F * static_cast<float>(i % 7));
        const float v = periodic_cellular(p, 4);
        EXPECT_GE(v, 0.0F);
        EXPECT_LE(v, 1.0F);
        EXPECT_NEAR(v, periodic_cellular(p + Vec3(4.0F, 4.0F, 0.0F), 4), 1e-5F);
    }
}

TEST(ParticleNoise, TheCurlVolumeIsDivergenceFreeAndTiles) {
    constexpr int n = 24;
    const std::vector<Vec3> v = curl_volume(n, 2, 2);
    ASSERT_EQ(v.size(), std::size_t{n} * n * n);
    const auto at = [&](int x, int y, int z) {
        const auto w = [](int i) { return (i % n + n) % n; };
        return v[static_cast<std::size_t>((w(z) * n + w(y)) * n + w(x))];
    };
    // Central differences across the volume, wrapping: the divergence
    // against the gradient's own size (each derivative's), mean over it.
    double div = 0.0;
    double grad = 0.0;
    for (int z = 0; z < n; ++z) {
        for (int y = 0; y < n; ++y) {
            for (int x = 0; x < n; ++x) {
                const float dxx = at(x + 1, y, z).x - at(x - 1, y, z).x;
                const float dyy = at(x, y + 1, z).y - at(x, y - 1, z).y;
                const float dzz = at(x, y, z + 1).z - at(x, y, z - 1).z;
                div += static_cast<double>(std::abs(dxx + dyy + dzz));
                grad += static_cast<double>(std::abs(dxx) + std::abs(dyy) + std::abs(dzz));
            }
        }
    }
    EXPECT_LT(div / grad, 0.1);  // sampled on a coarse grid: small, not zero
    float largest = 0.0F;
    for (const Vec3& c : v) {
        largest = std::max(largest, glm::length(c));
    }
    EXPECT_NEAR(largest, 1.0F, 1e-5F);
}

TEST(ParticleNoise, TheDetailVolumeIsInRange) {
    const std::vector<float> d = detail_volume(16, 2, 3);
    float lo = 1.0F;
    float hi = 0.0F;
    for (const float x : d) {
        lo = std::min(lo, x);
        hi = std::max(hi, x);
    }
    EXPECT_FLOAT_EQ(lo, 0.0F);  // stretched over the whole range
    EXPECT_FLOAT_EQ(hi, 1.0F);
}

// The renderer's volume's mean, which particles.slang's kDetailMean keeps
// the particles' mass with: change both together.
TEST(ParticleNoise, TheRenderersDetailVolumeHasTheShadersMean) {
    const std::vector<float> d = detail_volume(32, 4, 3);
    double sum = 0.0;
    for (const float x : d) {
        sum += static_cast<double>(x);
    }
    EXPECT_NEAR(sum / static_cast<double>(d.size()), 0.4586, 0.0005);
}

}  // namespace
}  // namespace fovea
