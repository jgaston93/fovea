#include <cmath>

#include "fovea/core/projection.hpp"
#include "fovea/render/renderer.hpp"

namespace fovea {

SceneView reference_scene_view(const ViewDesc& view, const ReferenceScene& scene) {
    // Double precision for the inverse; the GPU gets floats.
    const DMat4 inverse_projection =
        glm::inverse(off_axis_projection(view.projection.frustum, view.projection.near_m));
    // Body = local level frame: the eyepoint is level and facing north.
    const DMat4 eye_to_local = glm::mat4_cast(eye_to_body(view.pose));
    SceneView v;
    v.clip_to_local = Mat4(eye_to_local * inverse_projection);
    v.eye_height_m = static_cast<float>(scene.eye_height_m);
    v.grid_spacing_m = static_cast<float>(scene.grid_spacing_m);
    v.exposure = static_cast<float>(std::exp2(scene.exposure_ev));
    return v;
}

}  // namespace fovea
