#pragma once

// A View is the general rendering unit (architecture.md, "Core
// interfaces"): display channels and future sensor views are both Views.

#include <cstdint>
#include <string>

#include "fovea/core/attitude.hpp"
#include "fovea/core/config.hpp"
#include "fovea/core/math.hpp"
#include "fovea/core/scene.hpp"

namespace fovea {

// Where the view's eye is (D-011): attached to an entity and moving with
// it (a driver's or pilot's eye, a mounted camera), or fixed in the world
// (a tower, a survey viewpoint).
struct PoseSource {
    enum class Kind : std::uint8_t { entity, world };
    Kind kind = Kind::entity;
    EntityId entity = 0;    // kind entity: the entity it rides on
    DVec3 offset_m{0.0};    // kind entity: from the entity's origin, its body axes
    Euler rotation;         // kind entity: view axis relative to the entity's body
    DVec3 world_ecef{0.0};  // kind world: the eye's position
    DQuat world_orientation{1.0, 0.0, 0.0, 0.0};  // kind world: eye frame to ECEF
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
    std::uint32_t index = 0;  // presenter output, or offscreen image, index
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

// The display view for channel `index` of the config. Its target follows the
// presenter: its own output with multi_swapchain, output 0 (the one spanned
// surface) with spanned, an offscreen image with offscreen.
ViewDesc view_from_channel(const ChannelConfig& channel, const DisplayConfig& config,
                           std::size_t index);

// Eye-frame vectors to body-frame vectors, for an entity-attached view.
DQuat eye_to_body(const PoseSource& pose);

}  // namespace fovea
