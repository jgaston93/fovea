#pragma once

// A View is the general rendering unit (architecture.md, "Core
// interfaces"): display channels and future sensor views are both Views.

#include <cstdint>
#include <string>

#include "fovea/core/attitude.hpp"
#include "fovea/core/config.hpp"
#include "fovea/core/math.hpp"

namespace fovea {

// Where the view's eye is. Only the ownship's eye point for now; attached to
// an entity, or fixed in the world, come later.
struct PoseSource {
    enum class Kind : std::uint8_t { ownship_eye };
    Kind kind = Kind::ownship_eye;
    DVec3 offset_m{0.0};  // from the design eye point, body axes
    Euler rotation;       // view axis relative to the body
};

struct Projection {
    Frustum frustum;
    double near_m = 0.1;
};

enum class RenderMode : std::uint8_t { visual };

// Per-view post-processing after the linear HDR scene (decision 3).
struct PostChain {
    double exposure_ev = 0.0;
};

// What the view renders into.
struct OutputTarget {
    enum class Kind : std::uint8_t { presenter_output, offscreen };
    Kind kind = Kind::presenter_output;
    std::uint32_t index = 0;  // presenter output index
};

struct ViewDesc {
    std::string name;
    PoseSource pose;
    Projection projection;
    RenderMode mode = RenderMode::visual;
    PostChain post;
    OutputTarget target;
    std::uint32_t render_width = 0;  // independent of the output's resolution
    std::uint32_t render_height = 0;
};

// The display view for a configured channel, drawn to presenter output
// `output_index`.
ViewDesc view_from_channel(const ChannelConfig& channel, double near_m, std::uint32_t output_index);

// Eye-frame vectors to body-frame vectors for this view.
DQuat eye_to_body(const PoseSource& pose);

}  // namespace fovea
