#include <chrono>
#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/attitude.hpp"
#include "fovea/core/geodesy.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/render/view.hpp"
#include "support/alloc_counter.hpp"

namespace fovea {
namespace {

// Near Huntsville; any point would do.
ReferenceScene test_scene() {
    ReferenceScene s;
    s.origin = {34.6, -86.8, 190.0};
    s.eye_height_m = 2.0;
    s.grid_spacing_m = 100.0;
    return s;
}

ViewDesc test_view(Euler rotation = {}, DVec3 offset = DVec3(0.0)) {
    ViewDesc v;
    v.pose.kind = PoseSource::Kind::entity;
    v.pose.entity = 0;
    v.pose.rotation = rotation;
    v.pose.offset_m = offset;
    v.projection.frustum = {-30.0, 30.0, 20.0, -20.0};
    return v;
}

// Grid coordinates (the grid follows the Earth): metres north and east of
// the origin, from latitude and longitude scaled by the curvature radii at
// the origin; height above the origin's altitude.
Geodetic grid_point(const ReferenceScene& s, double north, double east, double height) {
    const double lat = glm::radians(s.origin.lat_deg);
    const double w = 1.0 - wgs84::kE2 * std::sin(lat) * std::sin(lat);
    const double m = wgs84::kA * (1.0 - wgs84::kE2) / (w * std::sqrt(w));
    const double n = wgs84::kA / std::sqrt(w) * std::cos(lat);
    return {s.origin.lat_deg + glm::degrees(north / m), s.origin.lon_deg + glm::degrees(east / n),
            s.origin.alt_m + height};
}

// An entity at a grid point, with an attitude relative to its own local
// level.
EntityState entity_at(const ReferenceScene& s, double north, double east, double height,
                      const Euler& attitude) {
    const Geodetic g = grid_point(s, north, east, height);
    EntityState e;
    e.id = 0;
    e.cur.ecef = geodetic_to_ecef(g);
    e.cur.orientation = body_to_ecef(g, attitude);
    return e;
}

// One view through the rig, starting from the no-host view (as FrameLoop
// starts).
SceneView ride(const ReferenceScene& s, const ViewDesc& view, const SceneSnapshot& snapshot,
               std::size_t* below = nullptr) {
    const ViewRig rig{std::span<const ViewDesc>(&view, 1), s, 0.0};
    SceneView out = reference_scene_view(view, s);
    const std::size_t n = ride_entities(rig, snapshot, std::span<SceneView>(&out, 1));
    if (below != nullptr) {
        *below = n;
    }
    return out;
}

void expect_same(const SceneView& a, const SceneView& b, float tol) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            EXPECT_NEAR(a.clip_to_local[c][r], b.clip_to_local[c][r], tol) << c << "," << r;
        }
    }
    EXPECT_NEAR(a.eye_height_m, b.eye_height_m, tol);
    EXPECT_NEAR(a.eye_offset_m.x, b.eye_offset_m.x, tol);
    EXPECT_NEAR(a.eye_offset_m.y, b.eye_offset_m.y, tol);
}

TEST(RideEntities, AViewWithoutItsEntityKeepsItsLastView) {
    const auto s = test_scene();
    const ViewDesc v = test_view({20.0, 5.0, 0.0}, {1.0, 2.0, -0.5});
    auto snapshot = std::make_unique<SceneSnapshot>();
    // Before the host's entity appears: the no-host view.
    expect_same(ride(s, v, *snapshot), reference_scene_view(v, s), 0.0F);
    // Once it was seen, a frame without it (or with it inactive) keeps the
    // last view rather than snapping back to the origin.
    const ViewRig rig{std::span<const ViewDesc>(&v, 1), s, 0.0};
    SceneView out = reference_scene_view(v, s);
    ASSERT_TRUE(snapshot->entities.push_back(entity_at(s, 500.0, 0.0, 50.0, {})));
    (void)ride_entities(rig, *snapshot, std::span<SceneView>(&out, 1));
    const SceneView seen = out;
    EXPECT_NEAR(seen.eye_height_m, 50.0F - 0.5F * -1.0F, 1e-3F);
    snapshot->entities[0].active = false;
    (void)ride_entities(rig, *snapshot, std::span<SceneView>(&out, 1));
    expect_same(out, seen, 0.0F);
    snapshot->entities.clear();
    (void)ride_entities(rig, *snapshot, std::span<SceneView>(&out, 1));
    expect_same(out, seen, 0.0F);
}

TEST(RideEntities, AnEntityAtTheNoHostPoseGivesTheSameView) {
    const auto s = test_scene();
    const ViewDesc v = test_view({20.0, 5.0, 3.0}, {1.0, 2.0, -0.5});
    auto snapshot = std::make_unique<SceneSnapshot>();
    ASSERT_TRUE(snapshot->entities.push_back(entity_at(s, 0.0, 0.0, s.eye_height_m, {})));
    expect_same(ride(s, v, *snapshot), reference_scene_view(v, s), 1e-5F);
}

TEST(RideEntities, MovingTheEntityMovesTheEye) {
    const auto s = test_scene();
    auto snapshot = std::make_unique<SceneSnapshot>();
    // 230 m north, 70 m east, 120 m up: the eye is 120 m above the ground,
    // and from the nearest grid crossing (every 100 m) 30 m north and 30 m
    // west.
    ASSERT_TRUE(snapshot->entities.push_back(entity_at(s, 230.0, 70.0, 120.0, {})));
    const SceneView view = ride(s, test_view(), *snapshot);
    EXPECT_NEAR(view.eye_height_m, 120.0F, 1e-5F);
    EXPECT_NEAR(view.eye_offset_m.x, 30.0F, 1e-5F);
    EXPECT_NEAR(view.eye_offset_m.y, -30.0F, 1e-5F);
}

TEST(RideEntities, EachRotationAxisTurnsTheViewLikeTheChannelRotation) {
    // Turning the entity turns the eye as a channel rotation of the same
    // angles does (the channel rotation's rendering is checked on the GPU).
    const auto s = test_scene();
    for (const Euler a : {Euler{90.0, 0.0, 0.0}, Euler{0.0, 30.0, 0.0}, Euler{0.0, 0.0, -40.0},
                          Euler{215.0, -12.0, 70.0}}) {
        auto snapshot = std::make_unique<SceneSnapshot>();
        ASSERT_TRUE(snapshot->entities.push_back(entity_at(s, 0.0, 0.0, s.eye_height_m, a)));
        SCOPED_TRACE(testing::Message() << a.yaw_deg << " " << a.pitch_deg << " " << a.roll_deg);
        expect_same(ride(s, test_view(), *snapshot), reference_scene_view(test_view(a), s), 1e-5F);
    }
}

TEST(RideEntities, TheChannelOffsetRidesTheBodyAxes) {
    // Facing east (yaw 90), an eye 10 m forward and 3 m up is 10 m east of
    // the entity and 3 m higher.
    const auto s = test_scene();
    auto snapshot = std::make_unique<SceneSnapshot>();
    ASSERT_TRUE(snapshot->entities.push_back(entity_at(s, 0.0, 0.0, 50.0, {90.0, 0.0, 0.0})));
    const SceneView view = ride(s, test_view({}, {10.0, 0.0, -3.0}), *snapshot);
    // To a millimetre: 10 m out along the level at 243 m above the
    // ellipsoid is 10 * R / (R + 243 m) = 9.9996 m along the ground below.
    EXPECT_NEAR(view.eye_height_m, 53.0F, 1e-3F);
    EXPECT_NEAR(view.eye_offset_m.x, 0.0F, 1e-3F);
    EXPECT_NEAR(view.eye_offset_m.y, 10.0F, 1e-3F);
}

TEST(RideEntities, PrecisionHolds100KilometresOut) {
    // An eye 100 km north and 100 km east, a whole number of grid cells out
    // and at the same height and local attitude, sees what an eye at the
    // origin sees: what the GPU gets stays small and exact.
    const auto s = test_scene();
    const Euler a{33.0, 4.0, -2.0};
    auto here = std::make_unique<SceneSnapshot>();
    auto far = std::make_unique<SceneSnapshot>();
    ASSERT_TRUE(here->entities.push_back(entity_at(s, 0.0, 0.0, 300.0, a)));
    ASSERT_TRUE(far->entities.push_back(entity_at(s, 100000.0, 100000.0, 300.0, a)));
    const SceneView near_view = ride(s, test_view(), *here);
    const SceneView far_view = ride(s, test_view(), *far);
    expect_same(near_view, far_view, 2e-5F);
    // The offset from the nearest grid crossing: under a millimetre.
    EXPECT_LT(std::abs(far_view.eye_offset_m.x), 1e-3F);
    EXPECT_LT(std::abs(far_view.eye_offset_m.y), 1e-3F);
}

TEST(RideEntities, EyesBelowTheGridAreCounted) {
    // A host 20 m below the origin's altitude: drawn from just above the
    // grid, and counted (FrameLoop's summary reports it).
    const auto s = test_scene();
    auto snapshot = std::make_unique<SceneSnapshot>();
    ASSERT_TRUE(snapshot->entities.push_back(entity_at(s, 0.0, 0.0, -20.0, {})));
    std::size_t below = 0;
    const SceneView view = ride(s, test_view(), *snapshot, &below);
    EXPECT_EQ(below, 1U);
    EXPECT_LT(view.eye_height_m, 0.01F);
    ASSERT_TRUE(snapshot->entities.push_back(entity_at(s, 0.0, 0.0, 20.0, {})));
    snapshot->entities[0] = snapshot->entities[1];
    (void)ride(s, test_view(), *snapshot, &below);
    EXPECT_EQ(below, 0U);
}

TEST(RideEntities, LevelFlightFarAwayStaysLevelAndAtItsHeight) {
    // A host flying wings level at a constant 300 m above sea level, at
    // increasing distances north of the origin (at 190 m): the eye stays
    // 110 m above the grid, and its horizon level, everywhere (the grid
    // follows the Earth's curve, as the ground does).
    const auto s = test_scene();
    auto here = std::make_unique<SceneSnapshot>();
    {
        EntityState e;
        const Geodetic g{s.origin.lat_deg, s.origin.lon_deg, 300.0};
        e.cur.ecef = geodetic_to_ecef(g);
        e.cur.orientation = body_to_ecef(g, Euler{});
        ASSERT_TRUE(here->entities.push_back(e));
    }
    const SceneView at_origin = ride(s, test_view(), *here);
    for (const double km : {20.0, 62.0, 100.0, 300.0}) {
        auto snapshot = std::make_unique<SceneSnapshot>();
        EntityState e;
        const Geodetic g{s.origin.lat_deg + km / 111.0, s.origin.lon_deg, 300.0};
        e.cur.ecef = geodetic_to_ecef(g);
        e.cur.orientation = body_to_ecef(g, Euler{});
        ASSERT_TRUE(snapshot->entities.push_back(e));
        const SceneView far_view = ride(s, test_view(), *snapshot);
        SCOPED_TRACE(testing::Message() << km << " km north");
        EXPECT_NEAR(far_view.eye_height_m, 110.0F, 1e-3F);
        // Level: the eye's rotation into the grid's frame is the same as at
        // the origin (to about 0.001 degrees).
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                EXPECT_NEAR(far_view.clip_to_local[c][r], at_origin.clip_to_local[c][r], 2e-5F);
            }
        }
    }
}

TEST(RideEntities, WorldFixedViewsStayPut) {
    const auto s = test_scene();
    ViewDesc v = test_view();
    v.pose.kind = PoseSource::Kind::world;
    const Geodetic tower{s.origin.lat_deg, s.origin.lon_deg, s.origin.alt_m + 40.0};
    v.pose.world_ecef = geodetic_to_ecef(tower);
    v.pose.world_orientation = body_to_ecef(tower, Euler{180.0, -10.0, 0.0});
    auto snapshot = std::make_unique<SceneSnapshot>();
    ASSERT_TRUE(snapshot->entities.push_back(entity_at(s, 500.0, 0.0, 50.0, {})));
    const SceneView view = ride(s, v, *snapshot);
    EXPECT_NEAR(view.eye_height_m, 40.0F, 1e-6F);
    expect_same(view, reference_scene_view(test_view({180.0, -10.0, 0.0}, {0.0, 0.0, -38.0}), s),
                1e-5F);
}

// An entity whose last two samples are 1/60 s apart, `move` metres north
// and `turn` degrees of yaw from the first to the second.
EntityState moving(const ReferenceScene& s, double move, double turn, LocalTime t1,
                   bool smooth = true) {
    const auto dt =
        std::chrono::duration_cast<LocalTime::duration>(std::chrono::duration<double>(1.0 / 60.0));
    EntityState prev = entity_at(s, 0.0, 0.0, 100.0, {10.0, 0.0, 0.0});
    EntityState cur = entity_at(s, move, 0.0, 100.0, {10.0 + turn, 0.0, 0.0});
    EntityState e = cur;
    e.prev = prev.cur;
    e.prev.sample_t = t1 - dt;
    e.cur.sample_t = t1;
    e.smooth = smooth;
    return e;
}

TEST(PoseAt, ExtrapolatesAtTheSamplesVelocity) {
    const auto s = test_scene();
    const LocalTime t1 = LocalTime{} + std::chrono::seconds(100);
    const EntityState e = moving(s, 1.0, 0.5, t1);  // 60 m/s north, 30 deg/s right
    // Two sample intervals past the newest: 2 m further, 1 degree more.
    const auto ahead =
        std::chrono::duration_cast<LocalTime::duration>(std::chrono::duration<double>(2.0 / 60.0));
    const Pose p = pose_at(e, t1 + ahead);
    const Geodetic g = ecef_to_geodetic(p.ecef);
    const Geodetic expected = grid_point(s, 3.0, 0.0, 100.0);
    EXPECT_NEAR(glm::length(p.ecef - geodetic_to_ecef(expected)), 0.0, 1e-3);
    const Euler a =
        to_euler(glm::inverse(glm::quat_cast(ned_to_ecef(g.lat_deg, g.lon_deg))) * p.orientation);
    EXPECT_NEAR(a.yaw_deg, 11.5, 1e-3);
    // Between the samples it interpolates.
    const Pose mid = pose_at(e, t1 - ahead / 4);
    EXPECT_NEAR(glm::length(mid.ecef - geodetic_to_ecef(grid_point(s, 0.5, 0.0, 100.0))), 0.0,
                1e-3);
}

TEST(PoseAt, HoldsWhenNotAllowedOrTooFarAhead) {
    const auto s = test_scene();
    const LocalTime t1 = LocalTime{} + std::chrono::seconds(100);
    const EntityState plain = moving(s, 1.0, 0.5, t1, false);
    EXPECT_EQ(pose_at(plain, t1 + std::chrono::milliseconds(30)).ecef, plain.cur.ecef);
    // A host gone quiet: extrapolation stops kMaxExtrapolationS past the
    // newest sample (6 m at 60 m/s), not at the frame's time.
    const EntityState e = moving(s, 1.0, 0.0, t1);
    const Pose far = pose_at(e, t1 + std::chrono::seconds(5));
    EXPECT_NEAR(glm::length(far.ecef - e.cur.ecef), 60.0 * kMaxExtrapolationS, 1e-3);
}

TEST(RideEntities, SmoothingRemovesTheJudderOfDriftingClocks) {
    // What the owner saw: a host turning steadily, sending at exactly 60 Hz,
    // drawn by frames at 59.95 Hz; samples land 0 or 2 to a frame. Without
    // smoothing the view's heading steps unevenly; with it, evenly.
    const auto s = test_scene();
    const ViewDesc v = test_view();
    const ViewRig rig{std::span<const ViewDesc>(&v, 1), s, 0.0};
    const double turn_dps = 12.0;
    auto spread = [&](bool smooth) {
        auto snapshot = std::make_unique<SceneSnapshot>();
        const LocalTime origin = LocalTime{} + std::chrono::seconds(100);
        auto at = [&](double seconds) {
            return origin + std::chrono::duration_cast<LocalTime::duration>(
                                std::chrono::duration<double>(seconds));
        };
        auto sample = [&](int k) {
            EntityState e = entity_at(s, 0.0, 0.0, 100.0, {turn_dps * k / 60.0, 0.0, 0.0});
            e.cur.sample_t = at(k / 60.0);
            return e.cur;
        };
        snapshot->entities.push_back(entity_at(s, 0.0, 0.0, 100.0, {}));
        EntityState& e = snapshot->entities[0];
        e.smooth = smooth;
        e.prev = sample(-1);
        e.cur = sample(0);
        int next = 1;
        double last_heading = 0.0;
        double min_step = 1e9;
        double max_step = -1e9;
        for (int f = 0; f < 600; ++f) {
            const double t = 0.002 + f / 59.95;  // frame starts
            while (next / 60.0 + 0.003 <= t) {   // samples that arrived (3 ms on the way)
                e.prev = e.cur;
                e.cur = sample(next++);
            }
            SceneView out = reference_scene_view(v, s);
            (void)ride_entities(rig, *snapshot, std::span<SceneView>(&out, 1), at(t));
            const Vec4 ray = out.clip_to_local * Vec4(0.0F, 0.0F, 1.0F, 1.0F);
            const double heading =
                glm::degrees(std::atan2(static_cast<double>(ray.y), static_cast<double>(ray.x)));
            if (f > 0) {
                min_step = std::min(min_step, heading - last_heading);
                max_step = std::max(max_step, heading - last_heading);
            }
            last_heading = heading;
        }
        return std::pair{min_step, max_step};
    };
    const double per_frame = turn_dps / 59.95;
    const auto [raw_min, raw_max] = spread(false);
    EXPECT_NEAR(raw_min, 0.0, 1e-6);                    // a repeated sample
    EXPECT_NEAR(raw_max, 2.0 * turn_dps / 60.0, 1e-4);  // two at once
    const auto [smooth_min, smooth_max] = spread(true);
    EXPECT_NEAR(smooth_min, per_frame, 1e-3);
    EXPECT_NEAR(smooth_max, per_frame, 1e-3);
}

TEST(RideEntities, AllocatesNothing) {
    const auto s = test_scene();
    std::vector<ViewDesc> views(3, test_view({10.0, 0.0, 0.0}));
    auto snapshot = std::make_unique<SceneSnapshot>();
    ASSERT_TRUE(snapshot->entities.push_back(entity_at(s, 10.0, 20.0, 30.0, {})));
    const ViewRig rig{views, s, 0.0};
    std::vector<SceneView> out(3);
    const test::AllocationCounter counter;
    for (int i = 0; i < 100; ++i) {
        ride_entities(rig, *snapshot, out);
    }
    EXPECT_EQ(counter.count(), 0U);
}

}  // namespace
}  // namespace fovea
