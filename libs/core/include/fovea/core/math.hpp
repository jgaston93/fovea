#pragma once

// Fovea's math types (D-010). Include glm through this header so every user
// gets the same configuration (set on the fovea_glm target).

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <numbers>

namespace fovea {

// CPU side: double precision for ECEF positions and attitudes.
using DVec3 = glm::dvec3;
using DMat3 = glm::dmat3;
using DMat4 = glm::dmat4;
using DQuat = glm::dquat;

// GPU side: camera-relative single precision.
using Vec3 = glm::vec3;
using Vec4 = glm::vec4;
using Mat4 = glm::mat4;

constexpr double deg_to_rad(double deg) {
    return deg * (std::numbers::pi / 180.0);
}

constexpr double rad_to_deg(double rad) {
    return rad * (180.0 / std::numbers::pi);
}

}  // namespace fovea
