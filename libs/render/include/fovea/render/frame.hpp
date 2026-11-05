#pragma once

#include <cstdint>

#include "fovea/core/scene.hpp"
#include "fovea/core/time.hpp"

namespace fovea {

// What every part of the renderer sees about the frame being built.
struct FrameContext {
    std::uint64_t index = 0;
    LocalTime start;                       // when the frame began
    LocalTime target_present;              // the deadline it is drawn for
    const SceneSnapshot* scene = nullptr;  // taken once at frame start
};

// Records GPU work for one view. Defined by the Vulkan backend (step 5);
// interfaces only pass it through.
class CommandRecorder;

}  // namespace fovea
