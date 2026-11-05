#include "fovea/core/projection.hpp"

#include <cassert>
#include <cmath>

namespace fovea {

DMat4 off_axis_projection(const Frustum& frustum, double near_m) {
    // The config loader enforces these; a hand-built Frustum{} (all zeros)
    // would otherwise give an inf/NaN matrix.
    assert(frustum.left_deg < frustum.right_deg && frustum.bottom_deg < frustum.top_deg);
    assert(frustum.left_deg > -90.0 && frustum.right_deg < 90.0);
    assert(frustum.bottom_deg > -90.0 && frustum.top_deg < 90.0);
    assert(near_m > 0.0);

    // Tangents of the edge angles. For an eye-frame point (x, y, z), the
    // horizontal tangent is y / x and the upward tangent is -z / x.
    const double l = std::tan(deg_to_rad(frustum.left_deg));
    const double r = std::tan(deg_to_rad(frustum.right_deg));
    const double t = std::tan(deg_to_rad(frustum.top_deg));
    const double b = std::tan(deg_to_rad(frustum.bottom_deg));

    // Row by row, with clip w = x (the distance along the view axis):
    //   clip.x = (2y - (r + l) x) / (r - l)  ->  NDC -1 at the left edge
    //   clip.y = (2z + (t + b) x) / (t - b)  ->  NDC -1 at the top edge
    //   clip.z = near                         ->  depth near / x
    // glm matrices are column-major: m[column][row].
    DMat4 m(0.0);
    m[0][0] = -(r + l) / (r - l);
    m[1][0] = 2.0 / (r - l);
    m[0][1] = (t + b) / (t - b);
    m[2][1] = 2.0 / (t - b);
    m[3][2] = near_m;
    m[0][3] = 1.0;
    return m;
}

}  // namespace fovea
