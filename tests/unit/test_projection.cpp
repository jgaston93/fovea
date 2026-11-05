#include <cmath>

#include <gtest/gtest.h>

#include "fovea/core/attitude.hpp"
#include "fovea/core/projection.hpp"

namespace fovea {
namespace {

constexpr double kTol = 1e-12;
constexpr double kNear = 0.1;

// NDC of an eye-frame point (x forward, y right, z down).
DVec3 ndc(const DMat4& m, const DVec3& p) {
    const glm::dvec4 clip = m * glm::dvec4(p, 1.0);
    return DVec3(clip) / clip.w;
}

// An eye-frame point at distance x along the view axis, in the direction
// with the given horizontal (right-positive) and vertical (up-positive)
// angles, measured as tangents from the axis.
DVec3 towards(double right_deg, double up_deg, double x = 5.0) {
    return {x, x * std::tan(deg_to_rad(right_deg)), -x * std::tan(deg_to_rad(up_deg))};
}

const Frustum kAsymmetric{-40.0, 25.0, 15.0, -20.0};

TEST(Projection, FrustumCornersMapToNdcCorners) {
    const DMat4 m = off_axis_projection(kAsymmetric, kNear);
    const DVec3 top_left = ndc(m, towards(-40.0, 15.0));
    const DVec3 bottom_right = ndc(m, towards(25.0, -20.0));
    const DVec3 bottom_left = ndc(m, towards(-40.0, -20.0));
    EXPECT_NEAR(top_left.x, -1.0, kTol);
    EXPECT_NEAR(top_left.y, -1.0, kTol);  // Vulkan: NDC y points down
    EXPECT_NEAR(bottom_right.x, 1.0, kTol);
    EXPECT_NEAR(bottom_right.y, 1.0, kTol);
    EXPECT_NEAR(bottom_left.x, -1.0, kTol);
    EXPECT_NEAR(bottom_left.y, 1.0, kTol);
}

TEST(Projection, SymmetricViewAxisIsTheCentre) {
    const DMat4 m = off_axis_projection({-30.0, 30.0, 17.0, -17.0}, kNear);
    const DVec3 c = ndc(m, {10.0, 0.0, 0.0});
    EXPECT_NEAR(c.x, 0.0, kTol);
    EXPECT_NEAR(c.y, 0.0, kTol);
}

TEST(Projection, ProjectedPositionDoesNotDependOnDistance) {
    const DMat4 m = off_axis_projection(kAsymmetric, kNear);
    const DVec3 near_point = ndc(m, towards(10.0, 5.0, 1.0));
    const DVec3 far_point = ndc(m, towards(10.0, 5.0, 1e6));
    EXPECT_NEAR(near_point.x, far_point.x, kTol);
    EXPECT_NEAR(near_point.y, far_point.y, kTol);
}

TEST(Projection, DepthIsReverseZWithAnInfiniteFarPlane) {
    const DMat4 m = off_axis_projection(kAsymmetric, kNear);
    EXPECT_NEAR(ndc(m, {kNear, 0.0, 0.0}).z, 1.0, kTol);
    EXPECT_NEAR(ndc(m, {2.0 * kNear, 0.0, 0.0}).z, 0.5, kTol);
    const double far_depth = ndc(m, {1e7, 0.0, 0.0}).z;
    EXPECT_GT(far_depth, 0.0);
    EXPECT_LT(far_depth, 1e-7);
    // Behind the eye: clip w is negative, so the point is clipped.
    EXPECT_LT((m * glm::dvec4(-1.0, 0.0, 0.0, 1.0)).w, 0.0);
}

// The example config's adjacent channels must meet without a step: a
// direction on their shared edge projects to the right edge of one channel
// and the left edge of the other, at the same height.
TEST(Projection, AdjacentChannelsMeetAtTheSharedEdge) {
    const Frustum f{-30.0, 30.0, 17.0, -17.0};
    const DMat4 m = off_axis_projection(f, kNear);
    // Eye frame from body: the inverse of the channel's view rotation.
    const DQuat left_view = glm::inverse(body_to_ned({-60.0, 0.0, 0.0}));
    const DQuat center_view = glm::inverse(body_to_ned({0.0, 0.0, 0.0}));
    for (double up_deg : {0.0, 10.0, -12.0}) {
        // Body direction 30 degrees left of the nose, up_deg above the horizon.
        const DVec3 d = body_to_ned({-30.0, up_deg, 0.0}) * DVec3(1.0, 0.0, 0.0);
        const DVec3 in_left = ndc(m, left_view * d);
        const DVec3 in_center = ndc(m, center_view * d);
        EXPECT_NEAR(in_left.x, 1.0, kTol) << up_deg;
        EXPECT_NEAR(in_center.x, -1.0, kTol) << up_deg;
        EXPECT_NEAR(in_left.y, in_center.y, kTol) << up_deg;
    }
}

}  // namespace
}  // namespace fovea
