#include <algorithm>
#include <chrono>
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

// The ground: the reference scene's grid follows the Earth's curve, at the
// origin's altitude, as real ground would. Its grid coordinates are
// distances north and east of the origin, from latitude and longitude
// (scaled by the curvature radii at the origin): lines run along parallels
// and meridians, exactly the grid spacing apart near the origin.
struct Ground {
    Geodetic origin;
    double north_m_per_rad = 0.0;  // meridian radius of curvature at the origin
    double east_m_per_rad = 0.0;   // prime vertical radius times cos(latitude)
};

Ground ground(const ReferenceScene& scene, double geoid_height_m) {
    Ground g;
    g.origin = {scene.origin.lat_deg, scene.origin.lon_deg, scene.origin.alt_m + geoid_height_m};
    const double lat = glm::radians(g.origin.lat_deg);
    const double w = 1.0 - wgs84::kE2 * std::sin(lat) * std::sin(lat);
    g.north_m_per_rad = wgs84::kA * (1.0 - wgs84::kE2) / (w * std::sqrt(w));
    g.east_m_per_rad = wgs84::kA / std::sqrt(w) * std::cos(lat);
    return g;
}

// Below this the eye counts as on the ground (the shader divides by the
// eye's height); such views are counted.
constexpr double kMinEyeHeightM = 1e-3;

// The difference of two longitudes, in (-180, 180].
double lon_difference(double a, double b) {
    double d = a - b;
    d -= 360.0 * std::round(d / 360.0);
    return d;
}

SceneView scene_view(const ViewDesc& view, const ReferenceScene& scene, const Ground& g,
                     const Eye& eye, bool* below_ground = nullptr) {
    // Double precision throughout; the GPU gets floats only of what stays
    // small: the eye's rotation against its own local level, its height, and
    // its offset from the nearest grid crossing (the grid repeats), so
    // precision holds any distance from the origin.
    const Geodetic at = ecef_to_geodetic(eye.ecef);
    const DMat3 ecef_to_level = glm::transpose(ned_to_ecef(at.lat_deg, at.lon_deg));
    const DMat3 eye_to_local = ecef_to_level * glm::mat3_cast(eye.to_ecef);
    const DMat4 inverse_projection =
        glm::inverse(off_axis_projection(view.projection.frustum, view.projection.near_m));
    const double height = at.alt_m - g.origin.alt_m;
    if (below_ground != nullptr) {
        *below_ground = height < kMinEyeHeightM;
    }
    const double north = glm::radians(at.lat_deg - g.origin.lat_deg) * g.north_m_per_rad;
    const double east =
        glm::radians(lon_difference(at.lon_deg, g.origin.lon_deg)) * g.east_m_per_rad;
    SceneView v;
    v.clip_to_local = Mat4(DMat4(eye_to_local) * inverse_projection);
    v.eye_height_m = static_cast<float>(std::max(height, kMinEyeHeightM));
    // To the nearest whole cell: within half a cell either way, and exactly
    // the eye's offset near the origin.
    const double cell = scene.grid_spacing_m;
    v.eye_offset_m = Vec3(static_cast<float>(north - std::round(north / cell) * cell),
                          static_cast<float>(east - std::round(east / cell) * cell), 0.0F);
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
    for (const ViewDesc& view : views_from_config(config)) {
        views.push_back(reference_scene_view(view, config.reference_scene, geoid));
    }
    return views;
}

Pose pose_at(const EntityState& e, LocalTime at) {
    const Pose& cur = e.cur;
    const Pose& prev = e.prev;
    const double span = std::chrono::duration<double>(cur.sample_t - prev.sample_t).count();
    // Smoothing needs the host's leave and two samples close enough together
    // to give a velocity (not a first sample, nor a gap of seconds).
    if (!e.smooth || span <= 1e-4 || span > 0.5) {
        return cur;
    }
    const double lead =
        std::min(std::chrono::duration<double>(at - cur.sample_t).count(), kMaxExtrapolationS);
    const double f = lead / span;  // negative: between the samples
    Pose p = cur;
    p.ecef = cur.ecef + (cur.ecef - prev.ecef) * f;
    // The rotation from the previous sample to the newest, continued by f
    // of itself (in ECEF axes, applied after the newest).
    DQuat step = cur.orientation * glm::inverse(prev.orientation);
    if (step.w < 0.0) {
        step = -step;  // the short way round
    }
    const double angle = 2.0 * std::acos(std::clamp(step.w, -1.0, 1.0));
    if (angle > 1e-12) {
        const DVec3 axis = glm::normalize(DVec3(step.x, step.y, step.z));
        p.orientation = glm::normalize(glm::angleAxis(angle * f, axis) * cur.orientation);
    }
    return p;
}

std::size_t ride_entities(const ViewRig& rig, const SceneSnapshot& snapshot,
                          std::span<SceneView> out, LocalTime at) {
    const Ground g = ground(rig.scene, rig.geoid_height_m);
    const std::size_t n = std::min(rig.views.size(), out.size());
    std::size_t below = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const ViewDesc& view = rig.views[i];
        Eye eye;
        if (view.pose.kind == PoseSource::Kind::world) {
            eye = {view.pose.world_ecef, view.pose.world_orientation};
        } else if (const EntityState* e = find_entity(snapshot, view.pose.entity);
                   e != nullptr && e->active) {
            const Pose p = pose_at(*e, at);
            eye = eye_on(view, p.ecef, p.orientation);
        } else {
            continue;  // keeps its last view: a missing entity doesn't snap it away
        }
        bool under = false;
        out[i] = scene_view(view, rig.scene, g, eye, &under);
        below += under ? 1 : 0;
    }
    return below;
}

std::vector<ViewDesc> views_from_config(const DisplayConfig& config) {
    std::vector<ViewDesc> views;
    views.reserve(config.channels.size());
    for (std::size_t i = 0; i < config.channels.size(); ++i) {
        views.push_back(view_from_channel(config.channels[i], config, i));
    }
    return views;
}

}  // namespace fovea
