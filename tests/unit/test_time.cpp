#include <chrono>

#include <gtest/gtest.h>

#include "fovea/core/time.hpp"

namespace fovea {
namespace {

using std::chrono::microseconds;
using std::chrono::seconds;

const LocalTime kStart{seconds{100}};
const Duration kPeriod{16'666'667};  // 1/60 s, rounded

TEST(PeriodicSchedule, OnTimeTicksSkipNothing) {
    PeriodicSchedule s(kStart, 60.0);
    for (std::int64_t i = 0; i < 600; ++i) {
        EXPECT_EQ(s.tick(s.deadline(i) + microseconds{50}), 0U);
    }
    EXPECT_EQ(s.ticks(), 600U);
    EXPECT_EQ(s.skipped(), 0U);
}

TEST(PeriodicSchedule, LateTickReportsTheDeadlinesItSkipped) {
    PeriodicSchedule s(kStart, 60.0);
    EXPECT_EQ(s.tick(kStart), 0U);
    // Next due at 1; starting at 3.5 periods skips 1 and 2.
    EXPECT_EQ(s.tick(kStart + kPeriod * 7 / 2), 2U);
    EXPECT_EQ(s.next_deadline(), s.deadline(4));
    EXPECT_EQ(s.skipped(), 2U);
}

TEST(PeriodicSchedule, EarlyTickIsOnTime) {
    PeriodicSchedule s(kStart, 60.0);
    EXPECT_EQ(s.tick(kStart), 0U);
    EXPECT_EQ(s.tick(s.deadline(1) - microseconds{200}), 0U);
    EXPECT_EQ(s.next_deadline(), s.deadline(2));
}

TEST(PeriodicSchedule, TickExactlyOnALaterDeadlineCountsIt) {
    PeriodicSchedule s(kStart, 60.0);
    EXPECT_EQ(s.tick(kStart), 0U);
    EXPECT_EQ(s.tick(s.deadline(3)), 2U);
    EXPECT_EQ(s.tick(s.deadline(4) - Duration{1}), 0U);
}

TEST(PeriodicSchedule, DeadlinesDoNotDrift) {
    PeriodicSchedule s(kStart, 60.0);
    EXPECT_EQ(s.deadline(60 * 3600), kStart + seconds{3600});
    PeriodicSchedule ntsc(kStart, 60000.0 / 1001.0);
    EXPECT_EQ(ntsc.deadline(60000), kStart + seconds{1001});
}

}  // namespace
}  // namespace fovea
