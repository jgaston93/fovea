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

// Round trips agree to a few nanometres from 1,000 km below the surface out
// to orbit, poles included. Accuracy falls off deeper down (measured: 5 um
// at 1,400 km from the centre, 1 cm at 370 km, metres inside 170 km); such
// positions are invalid input for an IG anyway. Longitude is 0 on the
// polar axis.
Geodetic ecef_to_geodetic(const DVec3& ecef);

// Rotation taking local North-East-Down vectors at (lat, lon) to ECEF. Its
// columns are the north, east and down unit vectors in ECEF.
DMat3 ned_to_ecef(double lat_deg, double lon_deg);

}  // namespace fovea
