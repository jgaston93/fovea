#pragma once

// The Vulkan renderer: device setup and the presenters, behind a header
// with no Vulkan types. It draws a clear per output (step 5) or the
// reference scene (step 6).

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fovea/core/config.hpp"
#include "fovea/core/math.hpp"
#include "fovea/core/scene.hpp"
#include "fovea/platform/platform.hpp"
#include "fovea/render/atmosphere.hpp"
#include "fovea/render/view.hpp"
#include "fovea/terrain/database.hpp"

namespace fovea {

enum class Validation : std::uint8_t {
    off,
    if_available,  // the Khronos validation layer when installed
    required,      // fail if the layer is missing
};

struct RendererOptions {
    Validation validation = Validation::if_available;
    // Samples per pixel of the scene pass (milestone 5, D-027): 1, 2, 4 or
    // 8, as the GPU can (every Vulkan GPU can do 4); a count it can't do
    // fails, naming those it can.
    std::uint32_t msaa = 4;
    // The frame clock's rate ([display] target_hz): exposure adapts by a
    // frame period each frame drawn (D-031).
    double frame_rate_hz = 60.0;
    // How cloud layers are drawn ([display] clouds, milestone 6 step 6).
    CloudModel clouds = CloudModel::layer;
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

    // For terrain (milestone 3 step 5). The eye in ECEF, in double: tiles
    // are placed relative to it on the CPU, so the GPU sees only small
    // offsets. ECEF directions to clip space: the eye's rotation and the
    // projection, for positions relative to the eye. The projection's
    // vertical extent in tangents (top minus bottom), for screen-space
    // error.
    DVec3 eye_ecef{0.0};
    Mat4 ecef_to_clip{1.0F};
    float tan_height = 1.0F;
};

// The reference scene for a view with no host: its body level and facing
// north, eye_height_m above the ground origin, and the view's own offset and
// rotation. `geoid_height_m`: the geoid above the ellipsoid ([host]).
SceneView reference_scene_view(const ViewDesc& view, const ReferenceScene& scene,
                               double geoid_height_m = 0.0);

// The reference scene for every channel of a config, in channel order (what
// the app draws, and the golden-image test).
std::vector<SceneView> reference_scene_views(const DisplayConfig& config);

// The display view for each channel of a config, in channel order.
std::vector<ViewDesc> views_from_config(const DisplayConfig& config);

// Views that ride the scene's entities (milestone 2 step 3): each frame,
// each view's eye is placed from its entity's latest pose and the channel's
// offset and rotation, in double precision. The reference grid follows the
// Earth's curve at the origin's altitude, so level flight at a constant
// altitude stays level and at its height at any distance. A world-fixed
// view stays put; a view whose entity is missing or inactive keeps its last
// view (the no-host pose until its entity first appears).
struct ViewRig {
    std::span<const ViewDesc> views;  // must outlive the rig's users
    ReferenceScene scene;
    double geoid_height_m = 0.0;
};

// Updates out[i] for each of the rig's views (as many as both hold), and
// returns how many eyes were at or below the grid (drawn from just above
// it). Entities are drawn for `at`, the time the frame will be seen
// (pose_at). No allocation: for the render thread.
std::size_t ride_entities(const ViewRig& rig, const SceneSnapshot& snapshot,
                          std::span<SceneView> out, LocalTime at);

// How far past its newest sample an entity is extrapolated, at most: past
// this (a host that stopped sending), it holds there.
inline constexpr double kMaxExtrapolationS = 0.1;

// An entity's pose at `at`. Where the host allows it (`allowed`, the
// scene's smoothing_allowed, and the entity's smooth), its last two samples
// are continued at their velocity to `at`: back no further than the earlier
// sample, ahead no more than kMaxExtrapolationS past the newer. Otherwise,
// and when the samples can't give a velocity (one sample, a gap of over half
// a second, timestamps on different clocks, or a jump faster than any
// vehicle: a host repositioning it), its newest sample.
Pose pose_at(const EntityState& entity, LocalTime at, bool allowed = true);

// A view's heading: the direction of its centre ray in the level frame,
// degrees clockwise from north.
double view_heading_deg(const SceneView& view);

// Terrain (milestone 3 steps 5 and 6): which tiles load, when they split,
// and how much of the GPU they may take.
struct TerrainOptions {
    int max_level = 30;              // tiles finer than this aren't loaded
    double pixel_error = 1.0;        // a tile splits where its error shows as more (D-016)
    std::uint32_t max_tiles = 4096;  // resident at once, roots included
    std::size_t budget_bytes = std::size_t{2} << 30;  // their heights and imagery
};

// What load_terrain did up front: the roots, and the placements they hold
// (milestone 4 step 3; their models load afterwards).
struct TerrainLoad {
    std::size_t tiles = 0;
    std::size_t placements = 0;
    std::size_t models = 0;  // distinct models placed
    std::size_t bytes = 0;   // heights and imagery uploaded
    double seconds = 0.0;
    std::vector<std::string> problems;  // tiles skipped or drawn without imagery, and why
};

// The terrain in one frame (step 6), for the stats log and overlay.
struct TerrainStats {
    std::uint32_t drawn = 0;     // tiles drawn, over all outputs
    std::uint32_t resident = 0;  // on the GPU
    std::uint32_t loading = 0;   // asked for, not yet arrived
    std::uint32_t uploads = 0;   // arrived this frame
    std::uint32_t requests = 0;  // asked for this frame
    // Tiles drawn coarser than pixel_error because their children aren't
    // there yet: what paging hasn't kept up with (drawn anyway: never a hole).
    std::uint32_t misses = 0;
    std::size_t bytes = 0;  // GPU memory the tiles take
    std::uint64_t evictions = 0;
    std::uint64_t failures = 0;          // tiles that couldn't be loaded (skipped)
    std::uint32_t models_loaded = 0;     // placed models on the GPU so far
    std::uint32_t placements_drawn = 0;  // over all outputs
    // Visible but not drawn: more primitives in a view than the draw
    // records hold (kMaxModelDraws).
    std::uint32_t placements_dropped = 0;
};

// Up to this many outputs get GPU timings.
inline constexpr std::size_t kMaxTimedOutputs = 8;

// Where the last draw's time went, and the GPU time of an earlier frame
// (GPU results are read once that frame's fence has signalled, two frames
// later, so nothing waits for them).
struct FrameTiming {
    double wait_ms = 0.0;           // blocked: present wait, the frame fence and image acquires
    double work_ms = 0.0;           // recording, submitting and presenting
    std::uint64_t gpu_frame = 0;    // the frame (frames_drawn() numbering) these GPU times are for
    std::uint32_t gpu_outputs = 0;  // how many are valid; 0 without timestamps
    std::array<float, kMaxTimedOutputs> gpu_ms{};  // per output: scene, tone map (and overlay)
};

// What the debug overlay shows for one frame (step 7; D-006). Plain data:
// the frame loop fills it without allocating.
struct OverlayContent {
    std::span<const float> intervals_ms;  // a ring of recent frame intervals
    std::size_t intervals_oldest = 0;     // index of the oldest in the ring
    float target_ms = 16.667F;            // the frame period aimed for
    std::uint64_t frames = 0;
    std::uint64_t missed = 0;
    double interval_mean_ms = 0.0;
    double interval_max_ms = 0.0;
    double work_mean_ms = 0.0;
    std::uint32_t gpu_outputs = 0;
    std::array<float, kMaxTimedOutputs> gpu_ms{};  // the latest GPU times per output
    std::string_view note;                         // an optional extra line
    float host_age_ms = -1.0F;  // the host's newest sample, at frame start; negative: no host
    TerrainStats terrain;       // the last frame's; all zero without terrain
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
    std::uint32_t draw_scene_frame(std::span<const SceneView> views,
                                   const OverlayContent* overlay = nullptr);

    // The sun and moon the frames drawn from now on are lit by (milestone 6,
    // D-029): the frame loop sets them each frame from the scene's date and
    // time. Until then, those of EnvironmentState's default date and time.
    void set_sky(const SkyBodies& sky);

    // The visibility, metres (milestone 6 step 3, D-030): the haze the
    // frames drawn from now on look through. Until set, EnvironmentState's
    // default. A change remakes the atmosphere's tables on the next frame.
    void set_visibility(double visibility_m);

    // The weather layers (milestone 6 step 6, D-034), heights above the
    // ellipsoid: those the frames drawn from now on show. Until set, none.
    // No allocation; at most kMaxWeatherLayers are kept.
    void set_weather(std::span<const WeatherLayer> layers);

    // The sky's tables and an output's light, as the last frame left them
    // (milestone 6 step 2). For tests: waits for the GPU, and allocates.
    SkyReadback read_back_sky(std::uint32_t output);

    // Shows the debug overlay (Dear ImGui) on one output, drawn after the
    // tone map whenever draw_scene_frame is given content. One output only,
    // never a sensor view; offscreen outputs are allowed for tests. Call
    // before the first draw. Uploads the font for every printable ASCII
    // glyph here, so drawing never waits for an upload. Throws
    // std::runtime_error for a bad output. If a swapchain rebuild changes
    // the overlay output's format, the overlay is no longer drawn.
    void enable_overlay(std::uint32_t output);

    // True once the overlay is enabled and its textures are on the GPU.
    bool overlay_ready() const;

    // Frames drawn without the overlay because ImGui wanted a texture
    // uploaded (new content, such as a glyph never drawn before). Drawing
    // never uploads: ImGui's backend would block the render thread with
    // vkQueueWaitIdle, so the overlay is left out of that frame instead.
    std::uint64_t overlay_skipped_frames() const;

    // Draws a terrain database from now on, in every view, over the
    // reference scene's sky and grid. The roots are loaded here (init time:
    // read, and their uploads waited for); every other tile is paged in as
    // views want it (step 6), by a paging thread this starts, which reads
    // the database (so the renderer keeps it). Roots that can't be used are
    // skipped and listed. Throws std::runtime_error when no root loads or
    // the GPU can't bind options.max_tiles; once loaded, it can't be loaded
    // again.
    TerrainLoad load_terrain(std::unique_ptr<TerrainDatabase> database,
                             const TerrainOptions& options = {});

    // The terrain in the last frame drawn (zeros without terrain).
    const TerrainStats& terrain_stats() const;
    // Why paged tiles couldn't be used, so far (not for the render thread:
    // it copies them under a lock).
    std::vector<std::string> terrain_problems() const;
    std::uint32_t terrain_tiles_drawn() const { return terrain_stats().drawn; }

    // Offscreen outputs only: the last frame's pixels, RGBA8, row by row.
    // Waits for the GPU to finish it.
    std::vector<std::uint8_t> read_back_rgba(std::uint32_t output);

    // The last draw's timing (no allocation; for the frame loop's stats).
    const FrameTiming& last_timing() const;

    // True when presentation paces drawing: an on-screen output with FIFO.
    // Otherwise (offscreen, mailbox, immediate) the caller must pace itself.
    bool paced_by_display() const;

    // Blocks until the last frame drawn is on the first output's display
    // (VK_KHR_present_wait), so the next frame can start right after a
    // vsync: evenly spaced and with the least latency. Waits at most
    // timeout_ms (a covered first window must not hold back the others).
    // Returns the time waited, in ms; returns at once (0) without present
    // wait support or when there is nothing queued to wait for.
    double wait_for_last_present(double timeout_ms);

    // Frames drawn so far (draw_clear_frame and draw_scene_frame calls): the
    // numbering FrameTiming::gpu_frame uses.
    std::uint64_t frames_drawn() const;

    // Whether wait_for_last_present can wait (the GPU and surface support it).
    bool has_present_wait() const;

    // Validation layer warnings and errors so far (each also printed).
    std::uint64_t validation_messages() const;

private:
    struct Impl;
    explicit Renderer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace fovea
