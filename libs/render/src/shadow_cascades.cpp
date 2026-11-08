#include "fovea/render/shadow_cascades.hpp"

#include <algorithm>
#include <cmath>

namespace fovea {

namespace {

// The nearest cascade starts at the eye; between, the splits are mostly
// logarithmic (where perspective spends its texels), a quarter uniform.
constexpr double kNearM = 1.0;
constexpr double kLogShare = 0.75;

}  // namespace

ShadowCascades fit_shadow_cascades(const DVec3& eye_ecef, const Mat4& ecef_to_clip,
                                   const DVec3& towards_light) {
    const DMat4 m(ecef_to_clip);
    // The view's axis: clip w is the distance along it.
    const DVec3 forward = glm::normalize(DVec3(m[0][3], m[1][3], m[2][3]));
    // The frustum's four edges, from the eye.
    const DMat4 inv = glm::inverse(m);
    std::array<DVec3, 4> edges{};
    for (int k = 0; k < 4; ++k) {
        const glm::dvec4 p =
            inv * glm::dvec4((k & 1) != 0 ? 1.0 : -1.0, (k & 2) != 0 ? 1.0 : -1.0, 1.0, 1.0);
        const DVec3 d = glm::normalize(DVec3(p) / p.w);
        // Scaled to unit distance along the axis.
        edges[static_cast<std::size_t>(k)] = d / glm::dot(d, forward);
    }
    // The light's frame: z toward it, x and y across, from a fixed axis
    // (ECEF's z, or x when the light is near the poles' line) so they don't
    // turn with the view.
    const DVec3 lz = glm::normalize(towards_light);
    const DVec3 hint = std::abs(lz.z) < 0.9 ? DVec3(0.0, 0.0, 1.0) : DVec3(1.0, 0.0, 0.0);
    const DVec3 lx = glm::normalize(glm::cross(hint, lz));
    const DVec3 ly = glm::cross(lz, lx);

    ShadowCascades out;
    double near = 0.0;
    for (std::uint32_t i = 0; i < kShadowCascades; ++i) {
        const double f = static_cast<double>(i + 1) / kShadowCascades;
        const double log_split = kNearM * std::pow(kShadowDistanceM / kNearM, f);
        const double uniform_split = kNearM + (kShadowDistanceM - kNearM) * f;
        const double far = kLogShare * log_split + (1.0 - kLogShare) * uniform_split;
        // The slice's eight corners, and the sphere around them.
        std::array<DVec3, 8> corners{};
        for (std::size_t k = 0; k < 4; ++k) {
            corners[k] = edges[k] * near;
            corners[k + 4] = edges[k] * far;
        }
        DVec3 centre(0.0);
        for (const DVec3& c : corners) {
            centre += c / 8.0;
        }
        double radius = 0.0;
        for (const DVec3& c : corners) {
            radius = std::max(radius, glm::length(c - centre));
        }
        // Whole metres' sixteenths: the radius depends only on the view's
        // shape, but rounding keeps float noise from changing the texel.
        radius = std::ceil(radius * 16.0) / 16.0;
        const double texel = 2.0 * radius / kShadowMapSize;
        // The centre on a texel grid fixed in the world: snapped in the
        // light's frame, in the ECEF position (double), then back relative
        // to the eye.
        const DVec3 world = eye_ecef + centre;
        const double sx = std::floor(glm::dot(world, lx) / texel) * texel;
        const double sy = std::floor(glm::dot(world, ly) / texel) * texel;
        const double sz = glm::dot(world, lz);
        const DVec3 snapped = lx * sx + ly * sy + lz * sz - eye_ecef;

        // Relative to the eye p maps to x = (p - c).lx / r, y likewise, and
        // z = (top - (p - c).lz) / (top - bottom): 0 at the casters furthest
        // toward the light, 1 at the sphere's far side.
        const double top = radius + kShadowCasterReachM;
        const double bottom = -radius;
        const double depth = top - bottom;
        DMat4 to_map(1.0);
        const DVec3 rx = lx / radius;
        const DVec3 ry = ly / radius;
        const DVec3 rz = -lz / depth;
        for (int c = 0; c < 3; ++c) {
            to_map[c][0] = rx[c];
            to_map[c][1] = ry[c];
            to_map[c][2] = rz[c];
            to_map[c][3] = 0.0;
        }
        to_map[3][0] = -glm::dot(snapped, rx);
        to_map[3][1] = -glm::dot(snapped, ry);
        to_map[3][2] = top / depth - glm::dot(snapped, rz);
        to_map[3][3] = 1.0;
        out.to_map[i] = to_map;
        out.far_m[i] = far;
        out.texel_m[i] = texel;
        near = far;
    }
    return out;
}

}  // namespace fovea
