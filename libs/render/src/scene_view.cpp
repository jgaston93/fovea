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
    // The channel's eye offset, in body axes = the level frame here: z is
    // down, so it lowers the eye; x and y shift where it looks from.
    v.eye_height_m = static_cast<float>(scene.eye_height_m - view.pose.offset_m.z);
    v.eye_offset_m = Vec3(view.pose.offset_m);
    v.grid_spacing_m = static_cast<float>(scene.grid_spacing_m);
    v.exposure = static_cast<float>(std::exp2(scene.exposure_ev));
    return v;
}

std::vector<SceneView> reference_scene_views(const DisplayConfig& config) {
    std::vector<SceneView> views;
    views.reserve(config.channels.size());
    for (std::size_t i = 0; i < config.channels.size(); ++i) {
        views.push_back(reference_scene_view(view_from_channel(config.channels[i], config, i),
                                             config.reference_scene));
    }
    return views;
}

}  // namespace fovea
