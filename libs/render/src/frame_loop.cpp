#include "fovea/render/frame_loop.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <utility>

namespace fovea {

namespace {

// A host whose newest sample is older than this has stopped sending.
constexpr double kLiveHostMs = 500.0;

double ms_between(LocalTime a, LocalTime b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

}  // namespace

FrameLoop::FrameLoop(Renderer& renderer, SceneState& scene, std::span<const SceneView> views,
                     FrameLoopOptions options, Logger* stats)
    : renderer_(renderer),
      scene_(scene),
      views_(views),
      options_(options),
      stats_(stats),
      start_(now()),
      schedule_(start_, options.rate_hz),
      first_renderer_frame_(renderer.frames_drawn()),
      display_clock_(options.rate_hz) {
    intervals_.fill(static_cast<float>(1000.0 / options.rate_hz));
    if (options.rig != nullptr) {
        if (options.rig->views.size() != views.size()) {
            throw std::invalid_argument("FrameLoop: the rig's views don't match the loop's");
        }
        // Sized once here: the per-frame update writes in place.
        rigged_.assign(views.begin(), views.end());
        views_ = rigged_;
    }
    if (stats_ != nullptr) {
        stats_->write(LogLevel::info,
                      "frame,t_s,interval_ms,wait_ms,work_ms,missed,scene_sequence,sample_age_ms,"
                      "recv_age_ms,heading_deg,gpu_frame,gpu_ms...");
    }
}

FrameLoop::~FrameLoop() {
    // Stops the thread; an exception it stored is dropped here (call stop()
    // to get it).
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

const FrameRecord& FrameLoop::step() {
    // Pacing. With present wait: until the last frame is on screen, so this
    // one starts right after a vsync (even spacing, least latency).
    // Presentation (FIFO) without it: by blocking inside the draw. Otherwise
    // (offscreen, mailbox): by the clock.
    if (renderer_.paced_by_display()) {
        // At most two periods: a covered first window can't stall the rest.
        (void)renderer_.wait_for_last_present(2000.0 / options_.rate_hz);
    } else if (frames_ > 0) {
        std::this_thread::sleep_until(schedule_.next_deadline());
    }
    const LocalTime started = now();
    (void)schedule_.tick(started);  // keeps the self-paced deadlines going
    if (options_.frame_starts != nullptr) {
        (void)options_.frame_starts->try_push({frames_, started});
    }

    // The latest published scene, taken once for the whole frame.
    const SceneSnapshot& scene = scene_.latest();
    if (options_.rig != nullptr) {
        // Moving things are drawn for when the frame will be seen: with
        // present-wait pacing, the next vsync (DisplayClock), not the
        // frame's own jittery start; otherwise the start.
        const LocalTime shown =
            renderer_.paced_by_display() ? display_clock_.frame_started(started) : started;
        summary_.views_below_ground += ride_entities(*options_.rig, scene, rigged_, shown);
    }

    // The overlay shows the stats up to the previous frame.
    const OverlayContent* overlay = nullptr;
    if (options_.overlay) {
        // Always the whole ring, so ImGui's draw list keeps one size (a graph
        // growing a point a frame grew its buffers until the ring filled);
        // slots not yet recorded hold the target period (set in the
        // constructor), a flat line at the target.
        overlay_.intervals_ms = intervals_;
        overlay_.intervals_oldest = next_interval_;
        overlay_.target_ms = static_cast<float>(1000.0 / options_.rate_hz);
        overlay_.frames = summary_.frames;
        overlay_.missed = summary_.missed;
        overlay_.interval_mean_ms = summary_.interval_mean_ms;
        overlay_.interval_max_ms = summary_.interval_max_ms;
        overlay_.work_mean_ms = summary_.work_mean_ms;
        overlay_.gpu_outputs = last_.gpu_outputs;
        overlay_.gpu_ms = last_.gpu_ms;
        overlay_.note = options_.overlay_note;
        overlay_.host_age_ms = last_.recv_age_ms > 0.0 && last_.recv_age_ms < kLiveHostMs
                                   ? static_cast<float>(last_.sample_age_ms)
                                   : -1.0F;
        overlay = &overlay_;
    }
    const std::uint32_t drawn = renderer_.draw_scene_frame(views_, overlay);
    if (drawn == 0) {
        // Nothing drawable (every window minimized): nothing paced the call.
        std::this_thread::sleep_until(schedule_.next_deadline());
    }

    const FrameTiming& t = renderer_.last_timing();
    FrameRecord r;
    r.frame = frames_;
    r.t_s = ms_between(start_, started) / 1000.0;
    r.interval_ms = frames_ > 0 ? ms_between(last_start_, started) : 0.0;
    r.wait_ms = t.wait_ms;
    r.work_ms = t.work_ms;
    const double period_ms = 1000.0 / options_.rate_hz;
    r.missed = r.interval_ms > options_.missed_after_periods * period_ms;
    r.scene_sequence = scene.sequence;
    if (options_.rig != nullptr && !options_.rig->views.empty()) {
        r.heading_deg = view_heading_deg(rigged_[0]);
        if (const EntityState* e = find_entity(scene, options_.rig->views[0].pose.entity)) {
            r.sample_age_ms = ms_between(e->cur.sample_t, started);
            r.recv_age_ms = ms_between(e->cur.recv_t, started);
        }
        // The means cover frames while the host is sending: not before its
        // first sample, nor after it stops (when the newest only ages).
        if (r.recv_age_ms > 0.0 && r.recv_age_ms < kLiveHostMs) {
            ++summary_.entity_frames;
            const auto k = static_cast<double>(summary_.entity_frames);
            summary_.sample_age_mean_ms += (r.sample_age_ms - summary_.sample_age_mean_ms) / k;
            summary_.recv_age_mean_ms += (r.recv_age_ms - summary_.recv_age_mean_ms) / k;
        }
    }
    // In this loop's numbering (draws before the loop started don't count).
    const bool ours = t.gpu_outputs > 0 && t.gpu_frame >= first_renderer_frame_;
    r.gpu_frame = ours ? t.gpu_frame - first_renderer_frame_ : 0;
    r.gpu_outputs = ours ? t.gpu_outputs : 0;
    r.gpu_ms = t.gpu_ms;
    last_start_ = started;
    if (frames_ > 0) {
        intervals_[next_interval_] = static_cast<float>(r.interval_ms);
        next_interval_ = (next_interval_ + 1) % intervals_.size();
    }
    ++frames_;
    record(r);
    log(r);
    last_ = r;
    frames_completed_.store(frames_, std::memory_order_release);
    return last_;
}

void FrameLoop::record(const FrameRecord& r) {
    FrameSummary& s = summary_;
    const auto n = static_cast<double>(s.frames);
    ++s.frames;
    s.missed += r.missed ? 1 : 0;
    // Running means; the first frame has no interval.
    if (r.frame > 0) {
        const auto k = static_cast<double>(r.frame);
        s.interval_mean_ms += (r.interval_ms - s.interval_mean_ms) / k;
        s.interval_max_ms = std::max(s.interval_max_ms, r.interval_ms);
    }
    s.work_mean_ms += (r.work_ms - s.work_mean_ms) / (n + 1.0);
    s.work_max_ms = std::max(s.work_max_ms, r.work_ms);
    // A frame's GPU times arrive two frames later; count each frame once.
    if (r.gpu_outputs > 0 && (s.gpu_samples == 0 || r.gpu_frame != last_.gpu_frame)) {
        s.gpu_outputs = r.gpu_outputs;
        ++s.gpu_samples;
        const auto g = static_cast<double>(s.gpu_samples);
        for (std::uint32_t i = 0; i < r.gpu_outputs; ++i) {
            const auto v = static_cast<double>(r.gpu_ms[i]);
            s.gpu_mean_ms[i] += (v - s.gpu_mean_ms[i]) / g;
            s.gpu_max_ms[i] = std::max(s.gpu_max_ms[i], v);
        }
    }
}

void FrameLoop::log(const FrameRecord& r) {
    if (stats_ == nullptr) {
        return;
    }
    // GPU times as one field list, formatted into a fixed buffer (no
    // allocation on the render path).
    std::array<char, 128> gpu{};
    std::size_t used = 0;
    for (std::uint32_t i = 0; i < r.gpu_outputs && used < gpu.size(); ++i) {
        const int n = std::snprintf(gpu.data() + used, gpu.size() - used, ",%.3f",
                                    static_cast<double>(r.gpu_ms[i]));
        used += n > 0 ? static_cast<std::size_t>(n) : 0;
    }
    stats_->write(LogLevel::info, "%llu,%.6f,%.3f,%.3f,%.3f,%d,%llu,%.3f,%.3f,%.4f,%llu%s",
                  static_cast<unsigned long long>(r.frame), r.t_s, r.interval_ms, r.wait_ms,
                  r.work_ms, r.missed ? 1 : 0, static_cast<unsigned long long>(r.scene_sequence),
                  r.sample_age_ms, r.recv_age_ms, r.heading_deg,
                  static_cast<unsigned long long>(r.gpu_frame), gpu.data());
}

void FrameLoop::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread([this] {
        // An exception must not escape the thread (that terminates the
        // process): the loop stops and stop() rethrows it to the owner.
        try {
            while (running_.load(std::memory_order_acquire)) {
                (void)step();
            }
        } catch (...) {
            error_ = std::current_exception();
            failed_.store(true, std::memory_order_release);
        }
    });
}

void FrameLoop::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    if (error_) {
        std::rethrow_exception(std::exchange(error_, nullptr));
    }
}

}  // namespace fovea
