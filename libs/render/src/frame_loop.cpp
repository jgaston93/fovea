#include "fovea/render/frame_loop.hpp"

#include <algorithm>
#include <cstdio>

namespace fovea {

namespace {

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
      schedule_(start_, options.rate_hz) {
    if (stats_ != nullptr) {
        stats_->write(
            LogLevel::info,
            "frame,t_s,interval_ms,wait_ms,work_ms,missed,scene_sequence,gpu_frame,gpu_ms...");
    }
}

FrameLoop::~FrameLoop() {
    stop();
}

const FrameRecord& FrameLoop::step() {
    // Pacing. With present wait: until the last frame is on screen, so this
    // one starts right after a vsync (even spacing, least latency).
    // Presentation (FIFO) without it: by blocking inside the draw. Otherwise
    // (offscreen, mailbox): by the clock.
    if (renderer_.paced_by_display()) {
        (void)renderer_.wait_for_last_present();
    } else if (frames_ > 0) {
        std::this_thread::sleep_until(schedule_.next_deadline());
    }
    const LocalTime started = now();
    (void)schedule_.tick(started);  // keeps the self-paced deadlines going

    // The latest published scene, taken once for the whole frame.
    const SceneSnapshot& scene = scene_.latest();

    const std::uint32_t drawn = renderer_.draw_scene_frame(views_);
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
    r.gpu_frame = t.gpu_frame;
    r.gpu_outputs = t.gpu_outputs;
    r.gpu_ms = t.gpu_ms;
    last_start_ = started;
    ++frames_;
    record(r);
    log(r);
    last_ = r;
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
    stats_->write(LogLevel::info, "%llu,%.6f,%.3f,%.3f,%.3f,%d,%llu,%llu%s",
                  static_cast<unsigned long long>(r.frame), r.t_s, r.interval_ms, r.wait_ms,
                  r.work_ms, r.missed ? 1 : 0, static_cast<unsigned long long>(r.scene_sequence),
                  static_cast<unsigned long long>(r.gpu_frame), gpu.data());
}

void FrameLoop::start() {
    if (running_.exchange(true)) {
        return;
    }
    thread_ = std::thread([this] {
        while (running_.load(std::memory_order_acquire)) {
            (void)step();
        }
    });
}

void FrameLoop::stop() {
    running_.store(false, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
}

}  // namespace fovea
