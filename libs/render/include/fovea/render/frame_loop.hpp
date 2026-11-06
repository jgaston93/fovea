#pragma once

// The render thread's loop (milestone 1 step 7): a fixed-rate frame clock
// that takes the latest scene state, draws every view and records each
// frame's timing. The steady-state frame allocates nothing, does no I/O and
// waits only on the display, the GPU or the clock (architecture.md, "Render
// path rules"). Rare events are exempt: a swapchain rebuild (a resize) waits
// for the device and allocates. The views are fixed for now: the scene
// state's sequence is recorded, and its entity poses drive the views once a
// host sends them (milestone 2).

#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <span>
#include <string_view>
#include <thread>
#include <vector>

#include "fovea/core/frame_events.hpp"
#include "fovea/core/log.hpp"
#include "fovea/core/scene.hpp"
#include "fovea/core/time.hpp"
#include "fovea/render/renderer.hpp"

namespace fovea {

struct FrameLoopOptions {
    FrameLoopOptions() = default;
    FrameLoopOptions(double rate) : rate_hz(rate) {}  // implicit: FrameLoopOptions{60.0}, {rate}

    double rate_hz = 60.0;
    // A frame starting more than this many periods after the previous one
    // counts as missed (architecture.md: 1.5). The gap is measured, not the
    // schedule: a display at 59.95 Hz drifts from a 60 Hz schedule without
    // missing anything.
    double missed_after_periods = 1.5;
    // Fill the debug overlay each frame (the renderer must have one enabled).
    bool overlay = false;
    std::string_view overlay_note;  // an extra overlay line (must outlive the loop)
    // Each frame's start goes here (for a host adapter's Start of Frame);
    // if the ring is full, the frame start is dropped. Must outlive the loop.
    FrameStartRing* frame_starts = nullptr;
    // Views riding the scene's entities: when set, the loop's views are
    // recomputed from each frame's scene (ride_entities), starting from the
    // ones given. Must outlive the loop, and have as many views as the loop
    // (the constructor throws std::invalid_argument otherwise).
    const ViewRig* rig = nullptr;
};

// Recent frame intervals for the overlay's graph.
inline constexpr std::size_t kOverlayHistory = 240;

// One frame's numbers: what goes into the stats log.
struct FrameRecord {
    std::uint64_t frame = 0;
    double t_s = 0.0;                  // frame start, since the loop started
    double interval_ms = 0.0;          // since the previous frame's start
    double wait_ms = 0.0;              // blocked on the GPU or vsync (FrameTiming::wait_ms)
    double work_ms = 0.0;              // recording, submitting, presenting
    bool missed = false;               // more than missed_after_periods after the previous frame
    std::uint64_t scene_sequence = 0;  // which SceneState publish it drew
    // With a rig, about the first view's entity (0 without one): how old
    // its newest sample was at frame start, on the host's clock mapped
    // across, and since it arrived; and the first view's heading.
    double sample_age_ms = 0.0;
    double recv_age_ms = 0.0;
    double heading_deg = 0.0;
    // GPU time per output of an earlier frame (FrameTiming); NaN-free, 0 when
    // there are none yet.
    std::uint64_t gpu_frame = 0;
    std::uint32_t gpu_outputs = 0;
    std::array<float, kMaxTimedOutputs> gpu_ms{};
};

// Running totals over the run, for the summary printed at exit.
struct FrameSummary {
    std::uint64_t frames = 0;
    std::uint64_t missed = 0;
    double interval_mean_ms = 0.0;
    double interval_max_ms = 0.0;
    double work_mean_ms = 0.0;
    double work_max_ms = 0.0;
    std::uint32_t gpu_outputs = 0;
    std::uint64_t gpu_samples = 0;
    std::array<double, kMaxTimedOutputs> gpu_mean_ms{};
    std::array<double, kMaxTimedOutputs> gpu_max_ms{};
    // With a rig: views drawn with their eye at or below the grid (a host
    // below the origin's altitude), counted per view per frame.
    std::uint64_t views_below_ground = 0;
    // With a rig, frames while the first view's entity has a live host (its
    // newest sample under half a second old), and the mean sample and
    // arrival ages over them (FrameRecord).
    std::uint64_t entity_frames = 0;
    double sample_age_mean_ms = 0.0;
    double recv_age_mean_ms = 0.0;
};

class FrameLoop {
public:
    // The renderer, scene and views must outlive the loop. With stats, every
    // frame's record goes to that logger as a CSV line (header first).
    FrameLoop(Renderer& renderer, SceneState& scene, std::span<const SceneView> views,
              FrameLoopOptions options = {}, Logger* stats = nullptr);
    ~FrameLoop();
    FrameLoop(const FrameLoop&) = delete;
    FrameLoop& operator=(const FrameLoop&) = delete;

    // One frame on the calling thread: paces to the next deadline (unless
    // presentation does), draws, and records. The thread below calls this in
    // a loop; tests call it directly.
    const FrameRecord& step();

    // Runs step() on a render thread until stop(). If a frame throws, the
    // thread ends, failed() turns true, and stop() rethrows the exception.
    void start();
    void stop();
    bool failed() const { return failed_.load(std::memory_order_acquire); }

    // Safe to read after stop(), or from the thread that calls step().
    const FrameSummary& summary() const { return summary_; }

    // Frames finished so far; safe from any thread, while the loop runs.
    std::uint64_t frames_completed() const {
        return frames_completed_.load(std::memory_order_acquire);
    }

private:
    void record(const FrameRecord& r);
    void log(const FrameRecord& r);

    Renderer& renderer_;
    SceneState& scene_;
    std::span<const SceneView> views_;  // what's drawn: the given views, or rigged_
    std::vector<SceneView> rigged_;     // with a rig: this frame's views
    FrameLoopOptions options_;
    Logger* stats_;
    LocalTime start_;
    PeriodicSchedule schedule_;
    LocalTime last_start_;
    std::uint64_t frames_ = 0;
    FrameRecord last_;
    FrameSummary summary_;
    std::array<float, kOverlayHistory> intervals_{};
    std::size_t next_interval_ = 0;  // the oldest, once the ring is full
    OverlayContent overlay_;
    std::uint64_t first_renderer_frame_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> frames_completed_{0};
    std::exception_ptr error_;
    std::thread thread_;
};

}  // namespace fovea
