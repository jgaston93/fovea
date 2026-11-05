#pragma once

#include <chrono>
#include <cstdint>

namespace fovea {

// Local monotonic time. Protocol (host) timestamps are a separate type,
// added with the CIGI adapter.
using Clock = std::chrono::steady_clock;
using LocalTime = Clock::time_point;
using Duration = std::chrono::nanoseconds;

inline LocalTime now() noexcept {
    return Clock::now();
}

// Deadlines of a fixed-rate loop, start + k / rate_hz, computed from the
// tick index so they never drift. Doesn't sleep: the caller waits (on vsync
// or a timer) and reports when each tick actually began.
class PeriodicSchedule {
public:
    PeriodicSchedule(LocalTime start, double rate_hz);

    // A tick began at t. Returns how many deadlines were skipped since the
    // previous tick (0 when on time or early) and moves to the next deadline.
    std::uint64_t tick(LocalTime t);

    LocalTime deadline(std::int64_t index) const;
    LocalTime next_deadline() const { return deadline(next_index_); }
    std::uint64_t ticks() const { return ticks_; }
    std::uint64_t skipped() const { return skipped_; }

private:
    LocalTime start_;
    double rate_hz_;
    std::int64_t next_index_ = 0;
    std::uint64_t ticks_ = 0;
    std::uint64_t skipped_ = 0;
};

}  // namespace fovea
