#include <gtest/gtest.h>

#include <cmath>

#include "fovea/core/geodesy.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/render/shadow_cascades.hpp"
#include "fovea/render/view.hpp"

namespace fovea {
namespace {

// A view at KHSV, 50 m up, looking north-east and a little down.
SceneView test_view(double yaw_deg = 45.0) {
    ReferenceScene scene;
    scene.origin = {34.6372, -86.7751, 190.0};
    scene.eye_height_m = 50.0;
    ViewDesc v;
    v.pose.rotation = {yaw_deg, -10.0, 0.0};
    v.projection.frustum = {-30.0, 30.0, 18.0, -18.0};
    return reference_scene_view(v, scene);
}

// The sun 30 degrees up in the south-east, ECEF.
DVec3 test_sun(const SceneView& v) {
    const Geodetic g = ecef_to_geodetic(v.eye_ecef);
    const double el = glm::radians(30.0);
    const double az = glm::radians(135.0);
    return ned_to_ecef(g.lat_deg, g.lon_deg) *
           DVec3(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), -std::sin(el));
}

glm::dvec4 to_map(const ShadowCascades& c, std::uint32_t i, const DVec3& rel) {
    return c.to_map[i] * glm::dvec4(rel, 1.0);
}

// Every slice of the view lies inside its cascade's map, and casters out
// toward the light (as far as kShadowCasterReachM) are still in its depth.
TEST(ShadowCascades, EachSliceAndItsCastersFitItsMap) {
    const SceneView v = test_view();
    const DVec3 sun = test_sun(v);
    const ShadowCascades c = fit_shadow_cascades(v.eye_ecef, v.ecef_to_clip, sun);
    const DMat4 inv = glm::inverse(DMat4(v.ecef_to_clip));
    const DVec3 forward =
        glm::normalize(DVec3(v.ecef_to_clip[0][3], v.ecef_to_clip[1][3], v.ecef_to_clip[2][3]));
    double near = 0.0;
    for (std::uint32_t i = 0; i < kShadowCascades; ++i) {
        for (const double d : {near, c.far_m[i]}) {
            for (const double x : {-1.0, 1.0}) {
                for (const double y : {-1.0, 1.0}) {
                    const glm::dvec4 p = inv * glm::dvec4(x, y, 1.0, 1.0);
                    const DVec3 dir = glm::normalize(DVec3(p) / p.w);
                    const DVec3 corner = dir * (d / glm::dot(dir, forward));
                    const glm::dvec4 m = to_map(c, i, corner);
                    EXPECT_LE(std::abs(m.x), 1.0 + 1e-9) << i;
                    EXPECT_LE(std::abs(m.y), 1.0 + 1e-9) << i;
                    EXPECT_GE(m.z, 0.0) << i;
                    EXPECT_LE(m.z, 1.0) << i;
                    // A caster further toward the light, as far as the reach.
                    const glm::dvec4 up = to_map(c, i, corner + sun * kShadowCasterReachM * 0.99);
                    EXPECT_GE(up.z, 0.0) << i;
                }
            }
        }
        near = c.far_m[i];
    }
    EXPECT_NEAR(c.far_m[kShadowCascades - 1], kShadowDistanceM, 1e-6);
    // Finer near the eye.
    EXPECT_LT(c.texel_m[0], c.texel_m[kShadowCascades - 1] / 10.0);
}

// A point fixed in the world moves across the map only by whole texels as
// the eye moves (by a fraction of one), so shadows don't crawl.
TEST(ShadowCascades, AFixedPointKeepsItsPlaceWithinATexel) {
    const SceneView v = test_view();
    const DVec3 sun = test_sun(v);
    const ShadowCascades a = fit_shadow_cascades(v.eye_ecef, v.ecef_to_clip, sun);
    const DVec3 point = v.eye_ecef + DVec3(30.0, 40.0, 10.0);
    for (const double step : {0.13, 0.41, 0.77}) {
        const DVec3 moved = v.eye_ecef + DVec3(step, -step * 0.5, step * 0.25) * a.texel_m[1];
        const ShadowCascades b = fit_shadow_cascades(moved, v.ecef_to_clip, sun);
        const glm::dvec4 pa = to_map(a, 1, point - v.eye_ecef);
        const glm::dvec4 pb = to_map(b, 1, point - moved);
        // In texels: the shift is a whole number.
        const double texels_x = (pb.x - pa.x) * kShadowMapSize / 2.0;
        const double texels_y = (pb.y - pa.y) * kShadowMapSize / 2.0;
        EXPECT_NEAR(texels_x, std::round(texels_x), 1e-3) << step;
        EXPECT_NEAR(texels_y, std::round(texels_y), 1e-3) << step;
    }
}

}  // namespace
}  // namespace fovea
