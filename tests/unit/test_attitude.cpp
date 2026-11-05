#include <cmath>

#include <gtest/gtest.h>

#include "fovea/core/attitude.hpp"

namespace fovea {
namespace {

constexpr double kTol = 1e-12;
constexpr double kDegTol = 1e-9;

const DVec3 kForward{1.0, 0.0, 0.0};
const DVec3 kRight{0.0, 1.0, 0.0};

// Difference between two angles, ignoring whole turns.
double angle_diff(double a_deg, double b_deg) {
    return std::abs(std::remainder(a_deg - b_deg, 360.0));
}

void expect_near(const DVec3& a, const DVec3& b) {
    EXPECT_NEAR(a.x, b.x, kTol);
    EXPECT_NEAR(a.y, b.y, kTol);
    EXPECT_NEAR(a.z, b.z, kTol);
}

TEST(Attitude, ZeroAttitudePointsNorthWingsLevel) {
    const DQuat q = body_to_ned({});
    expect_near(q * kForward, {1.0, 0.0, 0.0});
    expect_near(q * kRight, {0.0, 1.0, 0.0});
}

TEST(Attitude, PositiveYawTurnsClockwiseFromNorth) {
    expect_near(body_to_ned({90.0, 0.0, 0.0}) * kForward, {0.0, 1.0, 0.0});  // east
    expect_near(body_to_ned({180.0, 0.0, 0.0}) * kForward, {-1.0, 0.0, 0.0});
}

TEST(Attitude, PositivePitchIsNoseUp) {
    const double s = std::sqrt(0.5);
    expect_near(body_to_ned({0.0, 45.0, 0.0}) * kForward, {s, 0.0, -s});  // -down is up
}

TEST(Attitude, PositiveRollIsRightWingDown) {
    expect_near(body_to_ned({0.0, 0.0, 90.0}) * kRight, {0.0, 0.0, 1.0});
}

TEST(Attitude, YawIsAppliedBeforePitch) {
    // Heading east, nose up 30: the nose points east and up, not north.
    const DVec3 nose = body_to_ned({90.0, 30.0, 0.0}) * kForward;
    expect_near(nose, {0.0, std::cos(deg_to_rad(30.0)), -std::sin(deg_to_rad(30.0))});
}

TEST(Attitude, EulerRoundTrip) {
    for (double yaw = 0.0; yaw < 360.0; yaw += 37.0) {
        for (double pitch = -85.0; pitch <= 85.0; pitch += 17.0) {
            for (double roll = -175.0; roll <= 180.0; roll += 35.0) {
                const Euler back = to_euler(body_to_ned({yaw, pitch, roll}));
                EXPECT_LT(angle_diff(back.yaw_deg, yaw), kDegTol) << yaw;
                EXPECT_NEAR(back.pitch_deg, pitch, kDegTol);
                EXPECT_LT(angle_diff(back.roll_deg, roll), kDegTol) << roll;
                EXPECT_GE(back.yaw_deg, 0.0);
                EXPECT_LT(back.yaw_deg, 360.0);
                EXPECT_GT(back.roll_deg, -180.0);
                EXPECT_LE(back.roll_deg, 180.0);
            }
        }
    }
}

TEST(Attitude, EulerRangesAreNormalized) {
    const Euler e = to_euler(body_to_ned({-90.0, 0.0, 270.0}));
    EXPECT_NEAR(e.yaw_deg, 270.0, kDegTol);
    EXPECT_NEAR(e.roll_deg, -90.0, kDegTol);
}

TEST(Attitude, GimbalLockKeepsTheNoseDirection) {
    const Euler e = to_euler(body_to_ned({30.0, 90.0, 10.0}));
    EXPECT_NEAR(e.pitch_deg, 90.0, 1e-6);
    // Same orientation, whatever the split between yaw and roll.
    const DQuat a = body_to_ned({30.0, 90.0, 10.0});
    const DQuat b = body_to_ned(e);
    expect_near(a * kRight, b * kRight);
}

TEST(Attitude, BodyToEcefAtTheEquatorFacingNorthPointsAlongZ) {
    const DQuat q = body_to_ecef({0.0, 0.0, 0.0}, {});
    expect_near(q * kForward, {0.0, 0.0, 1.0});               // north
    expect_near(q * DVec3{0.0, 0.0, 1.0}, {-1.0, 0.0, 0.0});  // down, towards the centre
}

}  // namespace
}  // namespace fovea
