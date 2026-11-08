#include <array>
#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "fovea/render/resolve.hpp"

namespace fovea {
namespace {

TEST(Resolve, EqualSamplesResolveToThemselves) {
    const std::array<Vec3, 4> s = {Vec3(0.3F, 2.0F, 7.0F), Vec3(0.3F, 2.0F, 7.0F),
                                   Vec3(0.3F, 2.0F, 7.0F), Vec3(0.3F, 2.0F, 7.0F)};
    const Vec3 c = weighted_resolve(s);
    EXPECT_NEAR(c.x, 0.3F, 1e-6F);
    EXPECT_NEAR(c.y, 2.0F, 1e-6F);
    EXPECT_NEAR(c.z, 7.0F, 1e-6F);
}

TEST(Resolve, DarkSamplesAverageAsUsual) {
    // Weights near 1 where luminance is small: close to the plain mean.
    const std::array<Vec3, 4> s = {Vec3(0.0F), Vec3(0.0F), Vec3(0.02F), Vec3(0.02F)};
    EXPECT_NEAR(weighted_resolve(s).x, 0.01F, 2e-4F);
}

TEST(Resolve, ABrightSampleDoesNotSwampItsPixel) {
    // An edge: one sample a glint 1000 times brighter than the three grey
    // ones. A plain mean is 250 (all but white once tone-mapped, a jagged
    // edge); the weighted one stays near the grey's few units, about the
    // tone-mapped mean.
    const std::array<Vec3, 4> s = {Vec3(1000.0F), Vec3(0.5F), Vec3(0.5F), Vec3(0.5F)};
    const Vec3 c = weighted_resolve(s);
    EXPECT_GT(c.x, 0.5F);
    EXPECT_LT(c.x, 2.0F);
    // As much a glint as the tone-mapped samples say: y / (1 + y) of the
    // result is their mean of x / (1 + x), within a few percent.
    float mapped = 0.0F;
    for (const Vec3& v : s) {
        mapped += v.x / (1.0F + v.x) / 4.0F;
    }
    EXPECT_NEAR(c.x / (1.0F + c.x), mapped, 0.05F);
}

TEST(Resolve, NeverDividesByZeroOrOverflows) {
    const std::array<Vec3, 4> huge = {Vec3(1e6F), Vec3(1e6F), Vec3(1e6F), Vec3(0.0F)};
    const Vec3 c = weighted_resolve(huge);
    EXPECT_TRUE(std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z));
    EXPECT_GT(c.x, 0.0F);
    const float inf = std::numeric_limits<float>::infinity();
    const std::array<Vec3, 4> overflowed = {Vec3(inf), Vec3(0.5F), Vec3(0.5F), Vec3(0.5F)};
    const Vec3 o = weighted_resolve(overflowed);
    EXPECT_TRUE(std::isfinite(o.x)) << "a sample past a 16-bit float's range";
    EXPECT_GT(o.x, 0.5F);
    const std::array<Vec3, 4> black = {Vec3(0.0F), Vec3(0.0F), Vec3(0.0F), Vec3(0.0F)};
    EXPECT_EQ(weighted_resolve(black).x, 0.0F);
    EXPECT_EQ(weighted_resolve({}).x, 0.0F);  // no samples
}

TEST(Resolve, LuminanceIsRec709AndNeverNegative) {
    EXPECT_NEAR(luminance(Vec3(1.0F)), 1.0F, 1e-6F);
    EXPECT_NEAR(luminance(Vec3(0.0F, 1.0F, 0.0F)), 0.7152F, 1e-6F);
    EXPECT_EQ(luminance(Vec3(-1.0F)), 0.0F);
}

}  // namespace
}  // namespace fovea
