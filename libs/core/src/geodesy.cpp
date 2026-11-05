#include "fovea/core/geodesy.hpp"

#include <cmath>

namespace fovea {

namespace {

// Prime-vertical radius of curvature at geodetic latitude lat (radians).
double prime_vertical_radius(double sin_lat) {
    return wgs84::kA / std::sqrt(1.0 - wgs84::kE2 * sin_lat * sin_lat);
}

}  // namespace

DVec3 geodetic_to_ecef(const Geodetic& g) {
    const double lat = deg_to_rad(g.lat_deg);
    const double lon = deg_to_rad(g.lon_deg);
    const double sin_lat = std::sin(lat);
    const double cos_lat = std::cos(lat);
    const double n = prime_vertical_radius(sin_lat);
    return {(n + g.alt_m) * cos_lat * std::cos(lon), (n + g.alt_m) * cos_lat * std::sin(lon),
            (n * (1.0 - wgs84::kE2) + g.alt_m) * sin_lat};
}

Geodetic ecef_to_geodetic(const DVec3& ecef) {
    const double p = std::hypot(ecef.x, ecef.y);
    const double lon = (p == 0.0) ? 0.0 : std::atan2(ecef.y, ecef.x);

    // Fixed-point iteration on latitude, lat = atan2(z + e^2 N sin(lat), p).
    // It is well-conditioned at the poles (p = 0) and converges to double
    // precision within a few steps except deep inside the Earth, where it
    // slows (see geodesy.hpp); the fixed count keeps the cost constant.
    double lat = std::atan2(ecef.z, p * (1.0 - wgs84::kE2));
    for (int i = 0; i < 6; ++i) {
        const double sin_lat = std::sin(lat);
        lat = std::atan2(ecef.z + wgs84::kE2 * prime_vertical_radius(sin_lat) * sin_lat, p);
    }

    // Height along the ellipsoid normal; stable at every latitude.
    const double sin_lat = std::sin(lat);
    const double alt = p * std::cos(lat) + ecef.z * sin_lat -
                       wgs84::kA * std::sqrt(1.0 - wgs84::kE2 * sin_lat * sin_lat);
    return {rad_to_deg(lat), rad_to_deg(lon), alt};
}

DMat3 ned_to_ecef(double lat_deg, double lon_deg) {
    const double lat = deg_to_rad(lat_deg);
    const double lon = deg_to_rad(lon_deg);
    const double sl = std::sin(lat);
    const double cl = std::cos(lat);
    const double so = std::sin(lon);
    const double co = std::cos(lon);
    const DVec3 north{-sl * co, -sl * so, cl};
    const DVec3 east{-so, co, 0.0};
    const DVec3 down{-cl * co, -cl * so, -sl};
    return {north, east, down};
}

}  // namespace fovea
