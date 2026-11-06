#include <algorithm>
#include <chrono>
#include <cmath>
#include <random>

#include <gtest/gtest.h>

#include "fovea/render/display_clock.hpp"

namespace fovea {
namespace {

LocalTime at(double s) {
    return LocalTime{} + std::chrono::duration_cast<LocalTime::duration>(
                             std::chrono::duration<double>(100.0 + s));
}

double seconds_between(LocalTime a, LocalTime b) {
    return std::chrono::duration<double>(b - a).count();
}

// Frames start 0.1 to 2 ms after each vsync (as measured on the desk: a
// spread of about +-0.9 ms); the predicted display times must step evenly,
// at the display's real period, whatever the nominal rate.
void expect_steady(double display_hz, double nominal_hz) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> wake(0.0001, 0.002);
    DisplayClock clock(nominal_hz);
    const double period = 1.0 / display_hz;
    LocalTime last{};
    double min_step = 1e9;
    double max_step = 0.0;
    double raw_min = 1e9;
    double raw_max = 0.0;
    double last_start = 0.0;
    for (int k = 0; k < 3000; ++k) {
        const double start = k * period + wake(rng);
        const LocalTime shown = clock.frame_started(at(start));
        if (k > 1000) {  // after it settles
            const double step = seconds_between(last, shown);
            min_step = std::min(min_step, step);
            max_step = std::max(max_step, step);
            raw_min = std::min(raw_min, start - last_start);
            raw_max = std::max(raw_max, start - last_start);
            // Shown at the vsync after the one the frame followed: the grid
            // is fitted to the starts, so it sits the mean wake-up (about
            // 1 ms here) after the true vsyncs; a constant, so steps stay
            // even.
            const double late = seconds_between(at((k + 1) * period), shown);
            EXPECT_GT(late, 0.0);
            EXPECT_LT(late, 0.002);
        }
        last = shown;
        last_start = start;
    }
    EXPECT_GT(raw_max - raw_min, 0.003);  // the starts really do jitter (up to 3.8 ms)
    EXPECT_LT(max_step - min_step, 0.0003) << "display steps vary by under 0.3 ms";
    EXPECT_NEAR(clock.period_ms(), 1000.0 / display_hz, 0.002);
}

TEST(DisplayClock, StepsEvenlyThroughWakeUpJitter) {
    expect_steady(60.0, 60.0);
}

TEST(DisplayClock, FollowsADisplaySlowerThanNominal) {
    expect_steady(59.95, 60.0);  // the desk's Dell
}

TEST(DisplayClock, SkipsMissedVsyncsAndRelocksAfterAStall) {
    DisplayClock clock(60.0);
    const double p = 1.0 / 60.0;
    for (int k = 0; k < 100; ++k) {
        (void)clock.frame_started(at(k * p + 0.001));
    }
    // A missed frame: the start two vsyncs on is shown at the vsync after
    // (plus the fitted 1 ms wake-up).
    EXPECT_NEAR(seconds_between(at(102 * p), clock.frame_started(at(101 * p + 0.001))), 0.001,
                0.0002);
    // A long stall: it locks on again.
    const LocalTime shown = clock.frame_started(at(200 * p + 0.0075));
    EXPECT_NEAR(seconds_between(at(200 * p + 0.0075 + p), shown), 0.0, 1e-6);
}

}  // namespace
}  // namespace fovea
