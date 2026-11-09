#include "fovea/terrain/terrain_query.hpp"

#include <algorithm>
#include <cmath>

#include "fovea/core/geodesy.hpp"

namespace fovea {

bool TerrainQuery::line_of_sight(const DVec3& a, const DVec3& b) const {
    const DVec3 d = b - a;
    const double length = glm::length(d);
    if (length <= 2.0 * kEndpointTolerance) {
        return true;  // the whole segment is within the tolerance
    }
    const DVec3 unit = d / length;
    return !intersect({a + unit * kEndpointTolerance, unit}, length - 2.0 * kEndpointTolerance)
                .has_value();
}

std::optional<DVec3> TerrainQuery::normal_at(double lat_deg, double lon_deg) const {
    const std::optional<double> h = height_at(lat_deg, lon_deg);
    if (!h) {
        return std::nullopt;
    }
    // Degrees per metre north and east (the meridian and prime vertical
    // radii of curvature), the latter capped short of the poles.
    const double phi = glm::radians(lat_deg);
    const double w2 = 1.0 - wgs84::kE2 * std::sin(phi) * std::sin(phi);
    const double meridian = wgs84::kA * (1.0 - wgs84::kE2) / (w2 * std::sqrt(w2));
    const double parallel = wgs84::kA / std::sqrt(w2) * std::max(std::cos(phi), 1e-6);
    const double dlat = glm::degrees(kNormalStep / meridian);
    const double dlon = glm::degrees(kNormalStep / parallel);
    // A slope from the heights either side of the point, or from the point
    // to the side that has one.
    const auto slope = [&](double lat_a, double lon_a, double lat_b, double lon_b) {
        const std::optional<double> a = height_at(lat_a, lon_a);
        const std::optional<double> b = height_at(lat_b, lon_b);
        if (a && b) {
            return (*b - *a) / (2.0 * kNormalStep);
        }
        if (b) {
            return (*b - *h) / kNormalStep;
        }
        if (a) {
            return (*h - *a) / kNormalStep;
        }
        return 0.0;
    };
    const double east = slope(lat_deg, lon_deg - dlon, lat_deg, lon_deg + dlon);
    const double north = slope(lat_deg - dlat, lon_deg, lat_deg + dlat, lon_deg);
    return glm::normalize(DVec3{-east, -north, 1.0});
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
