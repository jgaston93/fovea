#include "fovea/terrain/terrain_query.hpp"

#include <cmath>

#include "fovea/core/geodesy.hpp"

namespace fovea {

bool TerrainQuery::line_of_sight(const DVec3& a, const DVec3& b) const {
    const DVec3 d = b - a;
    const double length = glm::length(d);
    if (length == 0.0) {
        return !intersect({a, DVec3{0.0, 0.0, 1.0}}, 0.0).has_value();
    }
    return !intersect({a, d}, length).has_value();
}

std::optional<double> ConstantHeightTerrain::height_at(double /*lat_deg*/,
                                                       double /*lon_deg*/) const {
    return height_m_;
}

std::optional<TerrainHit> ConstantHeightTerrain::intersect(const DRay& ray,
                                                           double max_distance_m) const {
    // Scale to the unit sphere: x^2/A^2 + y^2/A^2 + z^2/B^2 = 1. The scaling
    // is linear, so the ray parameter t means the same in both spaces.
    const DVec3 inv_axes{1.0 / (wgs84::kA + height_m_), 1.0 / (wgs84::kA + height_m_),
                         1.0 / (wgs84::kB + height_m_)};
    const DVec3 o = ray.origin * inv_axes;
    const DVec3 d = ray.direction * inv_axes;
    const double dir_length = glm::length(ray.direction);

    const double c = glm::dot(o, o) - 1.0;
    if (c <= 0.0) {
        return TerrainHit{ray.origin, 0.0, kUnknownMaterial};  // starts on or below
    }
    if (dir_length == 0.0) {
        return std::nullopt;
    }
    const double a = glm::dot(d, d);
    const double b = 2.0 * glm::dot(o, d);
    const double disc = b * b - 4.0 * a * c;
    if (disc < 0.0 || b >= 0.0) {
        return std::nullopt;  // misses, or points away from the surface
    }
    // The nearer root, in the form that avoids cancellation.
    const double t = 2.0 * c / (-b + std::sqrt(disc));
    const double distance = t * dir_length;
    if (distance > max_distance_m) {
        return std::nullopt;
    }
    return TerrainHit{ray.origin + t * ray.direction, distance, kUnknownMaterial};
}

}  // namespace fovea
