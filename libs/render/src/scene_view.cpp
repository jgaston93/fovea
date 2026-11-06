#include <algorithm>
#include <cmath>

#include "fovea/core/attitude.hpp"
#include "fovea/core/geodesy.hpp"
#include "fovea/core/projection.hpp"
#include "fovea/render/renderer.hpp"

namespace fovea {

namespace {

// The eye a view looks from: its position, and its axes (x forward, y right,
// z down) to ECEF.
struct Eye {
    DVec3 ecef{0.0};
    DQuat to_ecef{1.0, 0.0, 0.0, 0.0};
};

// The eye of a view riding a body (an entity) at this pose: the channel's
// offset and rotation are in the body's axes.
Eye eye_on(const ViewDesc& view, const DVec3& body_ecef, const DQuat& body_to_ecef) {
    return {body_ecef + body_to_ecef * view.pose.offset_m, body_to_ecef * eye_to_body(view.pose)};
}

// With no host: a body level and facing north, eye_height_m above the
// origin, as in milestone 1.
Eye default_eye(const ViewDesc& view, const ReferenceScene& scene, double geoid_height_m) {
    const Geodetic g{scene.origin.lat_deg, scene.origin.lon_deg,
                     scene.origin.alt_m + geoid_height_m + scene.eye_height_m};
    return eye_on(view, geodetic_to_ecef(g), body_to_ecef(g, Euler{}));
}

// The ground plane's frame: north-east-down at the origin.
struct Ground {
    DVec3 origin_ecef{0.0};
    DMat3 ecef_to_ned{1.0};
};

Ground ground(const ReferenceScene& scene, double geoid_height_m) {
    const Geodetic g{scene.origin.lat_deg, scene.origin.lon_deg,
                     scene.origin.alt_m + geoid_height_m};
    return {geodetic_to_ecef(g), glm::transpose(ned_to_ecef(g.lat_deg, g.lon_deg))};
}

// Below this the eye counts as on the ground (the shader divides by the
// eye's height).
constexpr double kMinEyeHeightM = 1e-3;

SceneView scene_view(const ViewDesc& view, const ReferenceScene& scene, const Ground& g,
                     const Eye& eye) {
    // Double precision throughout; the GPU gets floats only of what stays
    // small: the eye's rotation, its height, and its offset within one grid
    // cell (the grid repeats), so precision holds any distance from the
    // origin.
    const DVec3 ned = g.ecef_to_ned * (eye.ecef - g.origin_ecef);
    const DMat3 eye_to_local = g.ecef_to_ned * glm::mat3_cast(eye.to_ecef);
    const DMat4 inverse_projection =
        glm::inverse(off_axis_projection(view.projection.frustum, view.projection.near_m));
    SceneView v;
    v.clip_to_local = Mat4(DMat4(eye_to_local) * inverse_projection);
    v.eye_height_m = static_cast<float>(std::max(-ned.z, kMinEyeHeightM));
    const double cell = scene.grid_spacing_m;
    // To the nearest whole cell: within half a cell either way, and exactly
    // the eye's offset near the origin.
    v.eye_offset_m = Vec3(static_cast<float>(ned.x - std::round(ned.x / cell) * cell),
                          static_cast<float>(ned.y - std::round(ned.y / cell) * cell), 0.0F);
    v.grid_spacing_m = static_cast<float>(cell);
    v.exposure = static_cast<float>(std::exp2(scene.exposure_ev));
    return v;
}

}  // namespace

SceneView reference_scene_view(const ViewDesc& view, const ReferenceScene& scene,
                               double geoid_height_m) {
    return scene_view(view, scene, ground(scene, geoid_height_m),
                      default_eye(view, scene, geoid_height_m));
}

std::vector<SceneView> reference_scene_views(const DisplayConfig& config) {
    const double geoid = config.host ? config.host->geoid_height_m : 0.0;
    std::vector<SceneView> views;
    views.reserve(config.channels.size());
    for (std::size_t i = 0; i < config.channels.size(); ++i) {
        views.push_back(reference_scene_view(view_from_channel(config.channels[i], config, i),
                                             config.reference_scene, geoid));
    }
    return views;
}

void ride_entities(const ViewRig& rig, const SceneSnapshot& snapshot, std::span<SceneView> out) {
    const Ground g = ground(rig.scene, rig.geoid_height_m);
    const std::size_t n = std::min(rig.views.size(), out.size());
    for (std::size_t i = 0; i < n; ++i) {
        const ViewDesc& view = rig.views[i];
        Eye eye;
        if (view.pose.kind == PoseSource::Kind::world) {
            eye = {view.pose.world_ecef, view.pose.world_orientation};
        } else if (const EntityState* e = find_entity(snapshot, view.pose.entity);
                   e != nullptr && e->active) {
            eye = eye_on(view, e->cur.ecef, e->cur.orientation);
        } else {
            eye = default_eye(view, rig.scene, rig.geoid_height_m);
        }
        out[i] = scene_view(view, rig.scene, g, eye);
    }
}

}  // namespace fovea
