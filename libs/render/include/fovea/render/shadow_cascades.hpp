#pragma once

// Cascaded shadow maps' fitting (milestone 6 step 5, D-032): each view's
// distance split into kShadowCascades slices, each slice bounded by a
// sphere (its size doesn't change as the view turns) and seen from the
// light along an orthographic box, its centre snapped to whole texels in
// the world (so shadows don't crawl as the eye moves). CPU only: the
// renderer fills each view's lighting block with these.

#include <array>
#include <cstdint>

#include "fovea/core/math.hpp"

namespace fovea {

inline constexpr std::uint32_t kShadowCascades = 4;
inline constexpr std::uint32_t kShadowMapSize = 2048;  // texels a side, each cascade
inline constexpr double kShadowDistanceM = 6000.0;     // how far from the eye shadows reach
// How far toward the light, beyond a slice, casters are kept: a hill or a
// tower outside the view still shades into it.
inline constexpr double kShadowCasterReachM = 3000.0;

struct ShadowCascades {
    // Points relative to the eye (ECEF axes) to each cascade's map: x and y
    // -1 to 1 across it, z 0 (nearest the light) to 1.
    std::array<DMat4, kShadowCascades> to_map{};
    // Each cascade's far end, metres along the view's axis; and a texel's
    // width on the ground there, metres (for the receivers' bias).
    std::array<double, kShadowCascades> far_m{};
    std::array<double, kShadowCascades> texel_m{};
};

// The cascades for a view whose eye is at eye_ecef and whose matrix takes
// points relative to the eye to clip space (SceneView's), lit from
// `towards_light` (a unit vector, ECEF).
ShadowCascades fit_shadow_cascades(const DVec3& eye_ecef, const Mat4& ecef_to_clip,
                                   const DVec3& towards_light);

}  // namespace fovea
