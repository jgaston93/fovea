#pragma once

// When a frame will be on screen (milestone 2 step 5). With present-wait
// pacing a frame starts just after a vsync and appears at the next one; its
// start time jitters (how soon the thread wakes) but vsyncs don't. Drawing
// moving things for the start time would carry that jitter onto the screen,
// so poses are taken for the predicted display time instead: a steady grid
// of vsyncs fitted to the starts, which follows the display's real rate
// (59.95 Hz is not 60) without following each wake-up's noise.

#include <optional>

#include "fovea/core/time.hpp"

namespace fovea {

class DisplayClock {
public:
    // `nominal_hz`: the display rate to start from.
    explicit DisplayClock(double nominal_hz);

    // A frame began at `start`, just after a vsync. Returns when it will be
    // on screen: the vsync after the one it followed.
    LocalTime frame_started(LocalTime start);

    double period_ms() const;

private:
    std::optional<LocalTime> vsync_;  // the fitted vsync the last frame followed
    double period_s_;
};

}  // namespace fovea
