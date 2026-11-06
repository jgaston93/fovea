#include "fovea/render/display_clock.hpp"

#include <chrono>
#include <cmath>

namespace fovea {

namespace {

// How much of each frame's timing error goes into the fitted phase and
// period: small, so one late wake-up moves the grid by a few percent of its
// lateness, while a steady drift is followed within a second or so.
constexpr double kPhaseGain = 0.05;
constexpr double kPeriodGain = 0.002;
constexpr double kRelockFraction = 0.25;

LocalTime::duration seconds(double s) {
    return std::chrono::duration_cast<LocalTime::duration>(std::chrono::duration<double>(s));
}

}  // namespace

DisplayClock::DisplayClock(double nominal_hz) : period_s_(1.0 / nominal_hz) {}

double DisplayClock::period_ms() const {
    return period_s_ * 1000.0;
}

LocalTime DisplayClock::frame_started(LocalTime start) {
    if (vsync_) {
        // The vsync this frame followed, on the grid: the next one, or more
        // if frames were missed.
        const double since = std::chrono::duration<double>(start - *vsync_).count();
        const double vsyncs = std::max(1.0, std::round(since / period_s_));
        const LocalTime predicted = *vsync_ + seconds(vsyncs * period_s_);
        const double error = std::chrono::duration<double>(start - predicted).count();
        // Rounding to the nearest vsync keeps the error within half a
        // period; wake-ups are at most a couple of milliseconds late, so
        // more than a quarter means the beat moved (a stall, a new display
        // mode): lock on again below.
        if (std::abs(error) > kRelockFraction * period_s_) {
            vsync_.reset();
        } else {
            vsync_ = predicted + seconds(kPhaseGain * error);
            period_s_ += kPeriodGain * error / vsyncs;
        }
    }
    if (!vsync_) {
        vsync_ = start;
    }
    return *vsync_ + seconds(period_s_);
}

}  // namespace fovea
