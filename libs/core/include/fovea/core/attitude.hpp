#pragma once

#include "fovea/core/geodesy.hpp"
#include "fovea/core/math.hpp"

namespace fovea {

// Attitude as CIGI states it, in degrees. Body axes: x forward, y right,
// z down. Applied in the order yaw, pitch, roll (aerospace 3-2-1).
//   yaw:   heading, clockwise from true north, [0, 360)
//   pitch: positive nose up, [-90, 90]
//   roll:  positive right wing down, (-180, 180]
struct Euler {
    double yaw_deg = 0.0;
    double pitch_deg = 0.0;
    double roll_deg = 0.0;
};

// Rotation taking body-frame vectors to local NED.
DQuat body_to_ned(const Euler& e);

// Inverse of body_to_ned, normalized to the ranges above. At pitch +-90 the
// yaw and roll are not separable; the result puts all of it in yaw.
Euler to_euler(const DQuat& body_to_ned);

// Rotation taking body-frame vectors to ECEF, for a body at the given
// position with the given local attitude.
DQuat body_to_ecef(const Geodetic& position, const Euler& e);

}  // namespace fovea
