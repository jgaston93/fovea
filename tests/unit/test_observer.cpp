// D-057: the observer's camera.

#include <cmath>
#include <limits>

#include <gtest/gtest.h>

#include "fovea/core/attitude.hpp"
#include "fovea/core/geodesy.hpp"
#include "fovea/core/observer.hpp"

namespace fovea {
namespace {

// A target over Huntsville, and its local north, east and down in ECEF.
const Geodetic kAt{34.65, -86.78, 200.0};

struct Local {
    DVec3 north, east, down;
};
Local local_axes() {
    const DQuat ned = body_to_ecef(kAt, Euler{});
    return {ned * DVec3(1, 0, 0), ned * DVec3(0, 1, 0), ned * DVec3(0, 0, 1)};
}

TEST(Observer, StepsWrapAndClamp) {
    ObserverState s = observer_start(ObserverConfig{});
    EXPECT_EQ(s.mode, ObserverMode::orbit);
    s.azimuth_deg = 350.0;
    s = observer_step(s, {20.0, 0.0, 0.0}, 1.0);  // 20 degrees a second, for a second
    EXPECT_NEAR(s.azimuth_deg, 10.0, 1e-9);       // wrapped
    s = observer_step(s, {0.0, 1000.0, 0.0}, 1.0);
    EXPECT_EQ(s.elevation_deg, kMaxObserverElevationDeg);
    s = observer_step(s, {0.0, -1000.0, 0.0}, 1.0);
    EXPECT_EQ(s.elevation_deg, kMinObserverElevationDeg);
    const double d = s.distance_m;
    s = observer_step(s, {0.0, 0.0, std::log(2.0)}, 1.0);  // doubles in a second
    EXPECT_NEAR(s.distance_m, 2.0 * d, 1e-9);
    s = observer_step(s, {0.0, 0.0, 100.0}, 1.0);
    EXPECT_EQ(s.distance_m, kMaxObserverDistanceM);
    s = observer_step(s, {0.0, 0.0, -100.0}, 1.0);
    EXPECT_EQ(s.distance_m, kMinObserverDistanceM);
    const ObserverState before = s;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    s = observer_step(s, {nan, nan, nan}, nan, nan, nan, nan);  // nothing moves
    EXPECT_EQ(s.azimuth_deg, before.azimuth_deg);
    EXPECT_EQ(s.elevation_deg, before.elevation_deg);
    EXPECT_EQ(s.distance_m, before.distance_m);
    s = observer_step(s, {}, 0.0, 30.0, 5.0, std::log(3.0));  // nudges
    EXPECT_NEAR(s.azimuth_deg, before.azimuth_deg + 30.0, 1e-9);
    EXPECT_NEAR(s.elevation_deg, before.elevation_deg + 5.0, 1e-9);
    EXPECT_NEAR(s.distance_m, 3.0 * before.distance_m, 1e-9);
}

// Orbit at bearing 0: due north of the target, at the distance, looking at
// it, level (its right axis horizontal).
TEST(Observer, OrbitLooksAtTheTargetFromItsBearing) {
    const DVec3 target = geodetic_to_ecef(kAt);
    const Local l = local_axes();
    ObserverState s;
    s.mode = ObserverMode::orbit;
    s.distance_m = 100.0;
    s.azimuth_deg = 0.0;
    s.elevation_deg = 30.0;
    const ObserverEye eye = observer_eye(s, target, body_to_ecef(kAt, Euler{123.0, 4.0, 5.0}));
    const DVec3 off = eye.ecef - target;
    EXPECT_NEAR(glm::length(off), 100.0, 1e-6);
    EXPECT_NEAR(glm::dot(off, l.north), 100.0 * std::cos(glm::radians(30.0)), 1e-6);
    EXPECT_NEAR(glm::dot(off, l.east), 0.0, 1e-6);
    EXPECT_NEAR(-glm::dot(off, l.down), 100.0 * std::sin(glm::radians(30.0)), 1e-6);
    const DVec3 forward = eye.to_ecef * DVec3(1, 0, 0);
    EXPECT_NEAR(glm::dot(forward, glm::normalize(-off)), 1.0, 1e-9);  // at the target
    const DVec3 right = eye.to_ecef * DVec3(0, 1, 0);
    EXPECT_NEAR(glm::dot(right, l.down), 0.0, 1e-9);  // no roll
    // The target's own attitude doesn't matter in orbit: the same with another.
    const ObserverEye other = observer_eye(s, target, body_to_ecef(kAt, Euler{}));
    EXPECT_NEAR(glm::length(other.ecef - eye.ecef), 0.0, 1e-6);
}

// Tether: the bearing from the heading. Heading east, 180 is behind: west.
TEST(Observer, TetherFollowsTheHeading) {
    const DVec3 target = geodetic_to_ecef(kAt);
    const Local l = local_axes();
    ObserverState s;
    s.mode = ObserverMode::tether;
    s.distance_m = 50.0;
    s.azimuth_deg = 180.0;
    s.elevation_deg = 0.0;
    const ObserverEye eye = observer_eye(s, target, body_to_ecef(kAt, Euler{90.0, 10.0, -20.0}));
    const DVec3 off = eye.ecef - target;
    EXPECT_NEAR(glm::dot(off, l.east), -50.0, 1e-6);  // behind an east-heading target
    EXPECT_NEAR(glm::dot(off, l.north), 0.0, 1e-6);
    const DVec3 forward = eye.to_ecef * DVec3(1, 0, 0);
    EXPECT_NEAR(glm::dot(forward, l.east), 1.0, 1e-9);  // looking the way it heads
}

// Top-down: straight above, looking down, north at the top of the view
// (the camera's -z, its up, pointing north).
TEST(Observer, TopDownLooksStraightDownNorthUp) {
    const DVec3 target = geodetic_to_ecef(kAt);
    const Local l = local_axes();
    ObserverState s;
    s.mode = ObserverMode::top_down;
    s.distance_m = 300.0;
    s.azimuth_deg = 77.0;  // unused
    const ObserverEye eye = observer_eye(s, target, body_to_ecef(kAt, Euler{45.0, 0.0, 0.0}));
    EXPECT_NEAR(-glm::dot(eye.ecef - target, l.down), 300.0, 1e-6);
    EXPECT_NEAR(glm::dot(eye.to_ecef * DVec3(1, 0, 0), l.down), 1.0, 1e-9);
    EXPECT_NEAR(glm::dot(eye.to_ecef * DVec3(0, 0, -1), l.north), 1.0, 1e-9);
}

// The controls across threads (here one): rates held, nudges and requests
// taken once, a stall's step capped.
TEST(Observer, ControlsAdvanceOnce) {
    ObserverConfig start;
    start.azimuth_deg = 90.0;
    ObserverControls c(start);
    c.set_rates({10.0, 0.0, 0.0});
    EXPECT_NEAR(c.advance(0.05).azimuth_deg, 90.5, 1e-9);
    EXPECT_NEAR(c.advance(5.0).azimuth_deg, 91.5, 1e-9);  // capped at 0.1 s
    c.set_rates({});
    c.nudge(4.0, 0.0, 0.0);
    EXPECT_NEAR(c.advance(0.0).azimuth_deg, 95.5, 1e-9);
    EXPECT_NEAR(c.advance(0.0).azimuth_deg, 95.5, 1e-9);  // the nudge taken once
    c.request_mode(ObserverMode::top_down);
    EXPECT_EQ(c.advance(0.0).mode, ObserverMode::top_down);
    EXPECT_EQ(c.advance(0.0).mode, ObserverMode::top_down);  // kept
    c.request_reset();
    EXPECT_EQ(c.advance(0.0).mode, ObserverMode::orbit);
    EXPECT_NEAR(c.state().azimuth_deg, 90.0, 1e-9);
    // A reset takes all that came with it: a mode, rates, a nudge.
    c.request_mode(ObserverMode::tether);
    c.set_rates({10.0, 0.0, 0.0});
    c.nudge(5.0, 0.0, 0.0);
    c.request_reset();
    EXPECT_EQ(c.advance(0.05).mode, ObserverMode::orbit);
    EXPECT_NEAR(c.state().azimuth_deg, 90.0, 1e-9);
    EXPECT_NEAR(c.advance(0.05).azimuth_deg, 90.0, 1e-9);
}

}  // namespace
}  // namespace fovea
