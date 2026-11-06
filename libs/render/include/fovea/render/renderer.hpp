#pragma once

// The Vulkan renderer (milestone 1 step 5): device setup and the presenters,
// behind a header with no Vulkan types. Drawing is a clear per output for
// now; the reference scene arrives in step 6.

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fovea/core/config.hpp"
#include "fovea/platform/platform.hpp"

namespace fovea {

enum class Validation : std::uint8_t {
    off,
    if_available,  // the Khronos validation layer when installed
    required,      // fail if the layer is missing
};

struct RendererOptions {
    Validation validation = Validation::if_available;
};

struct GpuInfo {
    std::string name;
    std::string driver;
    bool discrete = false;
    std::uint32_t api_major = 0;
    std::uint32_t api_minor = 0;
    bool validation = false;  // the validation layer is active
};

struct Rgb8 {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

// An on-screen output: a window with its own swapchain.
struct WindowOutput {
    const Window* window = nullptr;
    PresentMode present_mode = PresentMode::fifo;
};

// An offscreen output: an image of this size, with no window or display.
struct OffscreenOutput {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// One Renderer at a time (Vulkan entry points are process-wide in volk);
// creating a second while one is alive throws std::runtime_error.
class Renderer {
public:
    // One swapchain per window (the multi_swapchain presenter). The windows
    // must have been created with WindowDesc::vulkan and outlive the
    // Renderer. Prefers a discrete GPU that can present to every window.
    // Throws std::runtime_error when no GPU fits.
    static std::unique_ptr<Renderer> for_windows(Platform& platform,
                                                 std::span<const WindowOutput> outputs,
                                                 const RendererOptions& options = {});

    // Offscreen images (the offscreen presenter): needs no windows and no
    // display, so it runs headless and on CPU drivers such as lavapipe.
    static std::unique_ptr<Renderer> offscreen(std::span<const OffscreenOutput> outputs,
                                               const RendererOptions& options = {});

    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    const GpuInfo& gpu() const;
    std::uint32_t output_count() const;

    // One frame: clears output i to colours[i] on the GPU, then presents
    // every on-screen output in a single present call. Waits only for the
    // frame two frames back to finish, and for vsync with FIFO. Returns the
    // number of outputs drawn. 0 means nothing could be drawn just now (every
    // window minimized, or its swapchain being rebuilt): nothing paced the
    // call, so the caller should wait a little before the next. After an
    // exception, the Renderer must be destroyed, not drawn with again.
    std::uint32_t draw_clear_frame(std::span<const Rgb8> colours);

    // Offscreen outputs only: the last frame's pixels, RGBA8, row by row.
    // Waits for the GPU to finish it.
    std::vector<std::uint8_t> read_back_rgba(std::uint32_t output);

    // Validation layer warnings and errors so far (each also printed).
    std::uint64_t validation_messages() const;

private:
    struct Impl;
    explicit Renderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace fovea
