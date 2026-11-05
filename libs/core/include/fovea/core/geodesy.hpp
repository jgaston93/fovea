#pragma once

#include "fovea/core/math.hpp"

namespace fovea {

// WGS-84 ellipsoid.
namespace wgs84 {
inline constexpr double kA = 6378137.0;            // semi-major axis, m
inline constexpr double kF = 1.0 / 298.257223563;  // flattening
inline constexpr double kB = kA * (1.0 - kF);      // semi-minor axis, m
inline constexpr double kE2 = kF * (2.0 - kF);     // first eccentricity squared
}  // namespace wgs84

// Geodetic position. Altitude is height above the WGS-84 ellipsoid, not MSL;
// the geoid offset is applied where MSL altitudes enter (the CIGI adapter).
struct Geodetic {
    double lat_deg = 0.0;
    double lon_deg = 0.0;
    double alt_m = 0.0;
};

DVec3 geodetic_to_ecef(const Geodetic& g);

// Exact to well under a millimetre everywhere, including at the poles and
// the Earth's centre. Longitude is 0 on the polar axis.
Geodetic ecef_to_geodetic(const DVec3& ecef);

// Rotation taking local North-East-Down vectors at (lat, lon) to ECEF. Its
// columns are the north, east and down unit vectors in ECEF.
DMat3 ned_to_ecef(double lat_deg, double lon_deg);

}  // namespace fovea
