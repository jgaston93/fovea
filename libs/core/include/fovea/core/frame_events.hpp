#pragma once

// Frame starts, from the render thread to whoever reports them to a host
// (the CIGI adapter sends a Start of Frame for each). A lock-free ring: the
// render thread pushes without waiting or allocating, and never sees a
// socket or a protocol (architecture.md: render never includes net).

#include <cstdint>

#include "fovea/core/containers.hpp"
#include "fovea/core/time.hpp"

namespace fovea {

struct FrameStart {
    std::uint64_t frame = 0;  // the frame loop's frame number
    LocalTime t;              // when the frame started
};

// About a second of frames at 60 Hz: if the reader stalls longer, the
// newest frame starts are dropped (the render thread never waits).
using FrameStartRing = SpscRing<FrameStart, 64>;

}  // namespace fovea
