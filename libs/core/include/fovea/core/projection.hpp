#pragma once

#include "fovea/core/config.hpp"
#include "fovea/core/math.hpp"

namespace fovea {

// Off-axis projection with reverse-Z and an infinite far plane, for Vulkan
// (architecture.md, "Coordinates and precision").
//
// It takes points in the channel's eye frame, which uses the body axes
// (x forward along the view axis, y right, z down), straight to clip space,
// so no separate axis swap is needed. Vulkan's conventions apply: NDC x
// runs left to right, NDC y top to bottom, and depth is 1 at the near plane,
// falling towards 0 at infinity. The frustum edges map exactly to NDC -1
// and +1.
//
// Computed in double precision; the renderer narrows it to float.
DMat4 off_axis_projection(const Frustum& frustum, double near_m);

}  // namespace fovea
