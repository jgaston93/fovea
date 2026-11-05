#include "fovea/core/attitude.hpp"

#include <algorithm>
#include <cmath>

namespace fovea {

namespace {

// Bring an angle in degrees to (-180, 180].
double wrap_180(double deg) {
    double r = std::remainder(deg, 360.0);
    if (r <= -180.0) {
        r += 360.0;
    }
    return r;
}

}  // namespace

DQuat body_to_ned(const Euler& e) {
    const DQuat yaw = glm::angleAxis(deg_to_rad(e.yaw_deg), DVec3{0.0, 0.0, 1.0});
    const DQuat pitch = glm::angleAxis(deg_to_rad(e.pitch_deg), DVec3{0.0, 1.0, 0.0});
    const DQuat roll = glm::angleAxis(deg_to_rad(e.roll_deg), DVec3{1.0, 0.0, 0.0});
    return yaw * pitch * roll;
}

Euler to_euler(const DQuat& body_to_ned) {
    // With R = Rz(yaw) Ry(pitch) Rx(roll), glm's m[col][row]:
    //   m[0][2] = -sin(pitch), m[1][2] = sin(roll) cos(pitch),
    //   m[2][2] = cos(roll) cos(pitch), m[0][1] = sin(yaw) cos(pitch),
    //   m[0][0] = cos(yaw) cos(pitch).
    const DMat3 m = glm::mat3_cast(glm::normalize(body_to_ned));
    const double sin_pitch = std::clamp(-m[0][2], -1.0, 1.0);
    const double pitch = std::asin(sin_pitch);

    double yaw = 0.0;
    double roll = 0.0;
    if (std::abs(sin_pitch) < 1.0 - 1e-12) {
        yaw = std::atan2(m[0][1], m[0][0]);
        roll = std::atan2(m[1][2], m[2][2]);
    } else {
        // Gimbal lock: only yaw - roll (pitch up) or yaw + roll (pitch down)
        // is defined; report it as yaw with zero roll.
        yaw = std::atan2(-m[1][0], m[1][1]);
    }

    // A yaw just below zero becomes 360 - tiny, which can round to 360.
    double yaw_deg = wrap_180(rad_to_deg(yaw));
    if (yaw_deg < 0.0) {
        yaw_deg += 360.0;
    }
    if (yaw_deg >= 360.0) {
        yaw_deg = 0.0;
    }
    return {yaw_deg, rad_to_deg(pitch), wrap_180(rad_to_deg(roll))};
}

DQuat body_to_ecef(const Geodetic& position, const Euler& e) {
    const DQuat ned = glm::quat_cast(ned_to_ecef(position.lat_deg, position.lon_deg));
    return glm::normalize(ned * body_to_ned(e));
}

}  // namespace fovea
