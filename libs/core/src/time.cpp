#include "fovea/core/time.hpp"

#include <cassert>
#include <cmath>

namespace fovea {

PeriodicSchedule::PeriodicSchedule(LocalTime start, double rate_hz)
    : start_(start), rate_hz_(rate_hz) {
    assert(rate_hz > 0.0);
}

LocalTime PeriodicSchedule::deadline(std::int64_t index) const {
    const double ns = std::round(static_cast<double>(index) * 1e9 / rate_hz_);
    return start_ + Duration{static_cast<Duration::rep>(ns)};
}

std::uint64_t PeriodicSchedule::tick(LocalTime t) {
    // The last deadline at or before t: estimated in floating point, then
    // corrected against deadline() itself so boundaries are exact.
    const auto elapsed = static_cast<double>((t - start_).count());
    auto latest = static_cast<std::int64_t>(std::floor(elapsed * rate_hz_ / 1e9));
    while (deadline(latest + 1) <= t) {
        ++latest;
    }
    while (latest >= 0 && deadline(latest) > t) {
        --latest;
    }

    std::uint64_t skipped = 0;
    if (latest > next_index_) {
        skipped = static_cast<std::uint64_t>(latest - next_index_);
        next_index_ = latest;
    }
    ++next_index_;
    ++ticks_;
    skipped_ += skipped;
    return skipped;
}

}  // namespace fovea
