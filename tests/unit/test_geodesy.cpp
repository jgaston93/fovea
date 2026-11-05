#include <cmath>

#include <gtest/gtest.h>

#include "fovea/core/geodesy.hpp"

namespace fovea {
namespace {

constexpr double kMetreTol = 1e-6;
constexpr double kDegTol = 1e-11;  // about 1 micrometre on the ground

void expect_near(const DVec3& a, const DVec3& b, double tol) {
    EXPECT_NEAR(a.x, b.x, tol);
    EXPECT_NEAR(a.y, b.y, tol);
    EXPECT_NEAR(a.z, b.z, tol);
}

TEST(Geodesy, EquatorAndPrimeMeridianIsOnTheXAxis) {
    expect_near(geodetic_to_ecef({0.0, 0.0, 0.0}), {wgs84::kA, 0.0, 0.0}, kMetreTol);
    expect_near(geodetic_to_ecef({0.0, 90.0, 0.0}), {0.0, wgs84::kA, 0.0}, kMetreTol);
    expect_near(geodetic_to_ecef({0.0, 180.0, 0.0}), {-wgs84::kA, 0.0, 0.0}, kMetreTol);
}

TEST(Geodesy, PolesAreOnTheZAxisAtTheSemiMinorAxis) {
    expect_near(geodetic_to_ecef({90.0, 0.0, 0.0}), {0.0, 0.0, wgs84::kB}, kMetreTol);
    expect_near(geodetic_to_ecef({-90.0, 45.0, 0.0}), {0.0, 0.0, -wgs84::kB}, kMetreTol);
}

TEST(Geodesy, AltitudeIsAlongTheEllipsoidNormal) {
    // At 45 degrees the normal is not the radial direction; the offset
    // between two altitudes must still be along the local "up".
    const Geodetic g{45.0, 30.0, 0.0};
    const DVec3 offset = geodetic_to_ecef({g.lat_deg, g.lon_deg, 1000.0}) - geodetic_to_ecef(g);
    const DVec3 up = -ned_to_ecef(g.lat_deg, g.lon_deg)[2];
    expect_near(offset, 1000.0 * up, kMetreTol);
}

TEST(Geodesy, RoundTripIsExact) {
    for (double lat = -89.5; lat <= 89.5; lat += 8.5) {
        for (double lon = -179.0; lon <= 180.0; lon += 23.0) {
            for (double alt : {-430.0, 0.0, 192.0, 3000.0, 30000.0, 400000.0}) {
                const Geodetic back = ecef_to_geodetic(geodetic_to_ecef({lat, lon, alt}));
                EXPECT_NEAR(back.lat_deg, lat, kDegTol) << lat << " " << lon << " " << alt;
                EXPECT_NEAR(back.lon_deg, lon, kDegTol) << lat << " " << lon << " " << alt;
                EXPECT_NEAR(back.alt_m, alt, kMetreTol) << lat << " " << lon << " " << alt;
            }
        }
    }
}

TEST(Geodesy, RoundTripNearKhsv) {
    // Huntsville International, about the field elevation.
    const Geodetic khsv{34.6372, -86.7751, 192.0};
    const Geodetic back = ecef_to_geodetic(geodetic_to_ecef(khsv));
    EXPECT_NEAR(back.lat_deg, khsv.lat_deg, kDegTol);
    EXPECT_NEAR(back.lon_deg, khsv.lon_deg, kDegTol);
    EXPECT_NEAR(back.alt_m, khsv.alt_m, kMetreTol);
}

TEST(Geodesy, RoundTripAtThePoles) {
    for (double lat : {90.0, -90.0}) {
        const Geodetic back = ecef_to_geodetic(geodetic_to_ecef({lat, 0.0, 500.0}));
        EXPECT_NEAR(back.lat_deg, lat, kDegTol);
        EXPECT_NEAR(back.alt_m, 500.0, kMetreTol);
    }
}

TEST(Geodesy, NedAxesAtTheEquator) {
    const DMat3 r = ned_to_ecef(0.0, 0.0);
    expect_near(r[0], {0.0, 0.0, 1.0}, 1e-15);   // north
    expect_near(r[1], {0.0, 1.0, 0.0}, 1e-15);   // east
    expect_near(r[2], {-1.0, 0.0, 0.0}, 1e-15);  // down
}

TEST(Geodesy, NedIsARightHandedRotation) {
    const DMat3 r = ned_to_ecef(34.6372, -86.7751);
    EXPECT_NEAR(glm::determinant(r), 1.0, 1e-14);
    expect_near(glm::cross(r[0], r[1]), r[2], 1e-14);
}

}  // namespace
}  // namespace fovea
