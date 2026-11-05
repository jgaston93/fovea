#include <cmath>

#include <gtest/gtest.h>

#include "fovea/core/geodesy.hpp"
#include "fovea/terrain/terrain_query.hpp"

namespace fovea {
namespace {

DVec3 up_at(double lat_deg, double lon_deg) {
    return -ned_to_ecef(lat_deg, lon_deg)[2];
}

TEST(ConstantHeightTerrain, HeightIsTheSameEverywhere) {
    const ConstantHeightTerrain terrain(192.0);
    EXPECT_EQ(terrain.height_at(34.6, -86.8), 192.0);
    EXPECT_EQ(terrain.height_at(-80.0, 170.0), 192.0);
}

// The surface is an ellipsoid with raised semi-axes, not exactly a constant
// height; the header states how far apart the two are (1.4 mm per km).
TEST(ConstantHeightTerrain, SurfaceStaysWithinTheStatedErrorOfItsHeight) {
    for (double h : {0.0, 192.0, 3000.0, 10000.0}) {
        const ConstantHeightTerrain terrain(h);
        double worst = 0.0;
        for (double lat = -90.0; lat <= 90.0; lat += 2.5) {
            const DVec3 above = geodetic_to_ecef({lat, 30.0, h + 5000.0});
            const auto hit = terrain.intersect({above, -up_at(lat, 30.0)}, 1e5);
            ASSERT_TRUE(hit.has_value()) << lat;
            worst = std::fmax(worst, std::abs(ecef_to_geodetic(hit->point).alt_m - h));
        }
        EXPECT_LT(worst, 1.5e-6 * h + 1e-6) << "height " << h;
    }
}

TEST(ConstantHeightTerrain, RayDownHitsAtTheRightDistance) {
    const ConstantHeightTerrain terrain(0.0);
    const DVec3 from = geodetic_to_ecef({0.0, 0.0, 1000.0});
    const auto hit = terrain.intersect({from, DVec3{-2.0, 0.0, 0.0}}, 5000.0);  // not unit length
    ASSERT_TRUE(hit.has_value());
    EXPECT_NEAR(hit->distance_m, 1000.0, 1e-6);
    EXPECT_NEAR(hit->point.x, wgs84::kA, 1e-6);
}

TEST(ConstantHeightTerrain, RaysUpOrTooShortMiss) {
    const ConstantHeightTerrain terrain(0.0);
    const DVec3 from = geodetic_to_ecef({45.0, 10.0, 1000.0});
    EXPECT_FALSE(terrain.intersect({from, up_at(45.0, 10.0)}, 1e7).has_value());
    EXPECT_FALSE(terrain.intersect({from, -up_at(45.0, 10.0)}, 999.0).has_value());
}

TEST(ConstantHeightTerrain, RayFromBelowHitsAtItsOrigin) {
    const ConstantHeightTerrain terrain(500.0);
    const DVec3 from = geodetic_to_ecef({10.0, 10.0, 100.0});
    const auto hit = terrain.intersect({from, up_at(10.0, 10.0)}, 1e4);
    ASSERT_TRUE(hit.has_value());
    EXPECT_EQ(hit->distance_m, 0.0);
}

TEST(ConstantHeightTerrain, LineOfSightOverTheHorizon) {
    // Two points 2 m up and 20 km apart along the surface: the Earth's
    // curvature (about 31 m of drop at 20 km) blocks the view. At 100 m up
    // it does not (the horizon is about 36 km away from each).
    const ConstantHeightTerrain terrain(0.0);
    const double dlon = 20000.0 / wgs84::kA * (180.0 / 3.14159265358979323846);
    EXPECT_FALSE(terrain.line_of_sight(geodetic_to_ecef({0.0, 0.0, 2.0}),
                                       geodetic_to_ecef({0.0, dlon, 2.0})));
    EXPECT_TRUE(terrain.line_of_sight(geodetic_to_ecef({0.0, 0.0, 100.0}),
                                      geodetic_to_ecef({0.0, dlon, 100.0})));
}

}  // namespace
}  // namespace fovea
