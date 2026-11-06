#pragma once

// Hides how finished images reach the displays (foundational decision 6):
// three swapchains, one spanned surface, or offscreen images. The renderer
// draws views; it never assumes which.

#include <cstdint>
#include <span>
#include <string>

#include "fovea/core/time.hpp"
#include "fovea/render/frame.hpp"

namespace fovea {

struct OutputDesc {
    std::string name;
    std::uint32_t width = 0;  // output resolution, not the views' render resolution
    std::uint32_t height = 0;
};

// When the last frame actually reached each output, for missed-frame and
// display-sync stats.
struct PresentTiming {
    std::uint64_t frame_index = 0;
    LocalTime presented;  // best estimate; exact with VK_KHR_present_wait
    bool exact = false;
};

class Presenter {
public:
    virtual ~Presenter() = default;
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    virtual std::span<const OutputDesc> outputs() const = 0;

    // Gets an image for every output for this frame.
    virtual void acquire(FrameContext& frame) = 0;
    // Presents every output, back to back.
    virtual void present(FrameContext& frame) = 0;
    virtual PresentTiming last_timing(std::uint32_t output) const = 0;

protected:
    Presenter() = default;
};

// Implementations, from step 5: MultiSwapchainPresenter, SpannedPresenter,
// OffscreenPresenter (headless and replay).

}  // namespace fovea
