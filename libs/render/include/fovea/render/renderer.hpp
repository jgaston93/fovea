#pragma once

// The Vulkan renderer: device setup and the presenters, behind a header
// with no Vulkan types. It draws a clear per output (step 5) or the
// reference scene (step 6).

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fovea/core/config.hpp"
#include "fovea/core/math.hpp"
#include "fovea/platform/platform.hpp"
#include "fovea/render/view.hpp"

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

// An on-screen output: a window with its own swapchain. The scene renders
// at render_width x render_height (0: the window's size when the Renderer is
// made; it stays that size if the window is resized) and is scaled to the
// window by the tone-map pass.
struct WindowOutput {
    const Window* window = nullptr;
    PresentMode present_mode = PresentMode::fifo;
    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;
};

// An offscreen output: an image of this size, with no window or display.
// The scene renders at the same size.
struct OffscreenOutput {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

// What one output's view of the reference scene needs (step 6).
struct SceneView {
    // Clip space to the local level frame (x north, y east, z down): the
    // eye's orientation times the inverse of the channel's projection.
    Mat4 clip_to_local{1.0F};
    float eye_height_m = 2.0F;  // the eye above the ground, offset included
    float grid_spacing_m = 10.0F;
    Vec3 eye_offset_m{0.0F};  // the eye's offset in the level frame (z unused)
    float exposure = 1.0F;    // linear multiplier before tone mapping
};

// The reference scene for a view, with the eyepoint level and facing north
// (no host moves it until milestone 2) and the view's own rotation.
SceneView reference_scene_view(const ViewDesc& view, const ReferenceScene& scene);

// Up to this many outputs get GPU timings.
inline constexpr std::size_t kMaxTimedOutputs = 8;

// Where the last draw's time went, and the GPU time of an earlier frame
// (GPU results are read once that frame's fence has signalled, two frames
// later, so nothing waits for them).
struct FrameTiming {
    double wait_ms = 0.0;           // blocked: the frame fence and image acquires (vsync with FIFO)
    double work_ms = 0.0;           // recording, submitting and presenting
    std::uint64_t gpu_frame = 0;    // the frame the GPU times below belong to
    std::uint32_t gpu_outputs = 0;  // how many are valid; 0 without timestamps
    std::array<float, kMaxTimedOutputs> gpu_ms{};  // per output: scene plus tone map
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

    // One frame of the reference scene: each output renders its view in
    // linear HDR at its render size, then the tone-map pass draws it into
    // the output. Presents and paces like draw_clear_frame, and returns the
    // same count.
    std::uint32_t draw_scene_frame(std::span<const SceneView> views);

    // Offscreen outputs only: the last frame's pixels, RGBA8, row by row.
    // Waits for the GPU to finish it.
    std::vector<std::uint8_t> read_back_rgba(std::uint32_t output);

    // The last draw's timing (no allocation; for the frame loop's stats).
    const FrameTiming& last_timing() const;

    // True when presentation paces drawing: an on-screen output with FIFO.
    // Otherwise (offscreen, mailbox, immediate) the caller must pace itself.
    bool paced_by_display() const;

    // Validation layer warnings and errors so far (each also printed).
    std::uint64_t validation_messages() const;

private:
    struct Impl;
    explicit Renderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace fovea
