#include <stdexcept>

#include <gtest/gtest.h>

#include "fovea/platform/platform.hpp"

namespace fovea {
namespace {

// SDL's dummy video driver: a fake display and windows, so these run on CI
// machines without one. Real displays are checked on hardware (step 4).
PlatformOptions dummy() {
    return {"dummy"};
}

TEST(Platform, StartsAndListsMonitors) {
    const Platform platform(dummy());
    EXPECT_EQ(platform.video_driver(), "dummy");
    ASSERT_FALSE(platform.monitors().empty());
    int primaries = 0;
    for (const MonitorInfo& m : platform.monitors()) {
        EXPECT_GT(m.width, 0U);
        EXPECT_GT(m.height, 0U);
        primaries += m.primary ? 1 : 0;
    }
    EXPECT_EQ(primaries, 1);
}

TEST(Platform, OnlyOneAtATime) {
    const Platform first(dummy());
    EXPECT_THROW(Platform second(dummy()), std::runtime_error);
}

TEST(Platform, CanBeStartedAgainAfterTheFirstIsGone) {
    { const Platform first(dummy()); }
    EXPECT_NO_THROW(Platform second(dummy()));
}

TEST(Platform, FullscreenWindowCoversItsMonitor) {
    Platform platform(dummy());
    const MonitorInfo& m = platform.monitors().at(0);
    const Window w = platform.create_window({"test", 0, true, 0, 0});
    EXPECT_EQ(w.pixel_width(), static_cast<std::uint32_t>(m.width * m.pixel_density));
    EXPECT_EQ(w.pixel_height(), static_cast<std::uint32_t>(m.height * m.pixel_density));
    EXPECT_EQ(w.current_monitor(), 0);
}

TEST(Platform, WindowedSizeIsHonoured) {
    Platform platform(dummy());
    const Window w = platform.create_window({"test", 0, false, 640, 360});
    EXPECT_EQ(w.pixel_width(), 640U);
    EXPECT_EQ(w.pixel_height(), 360U);
}

TEST(Platform, MissingMonitorThrows) {
    Platform platform(dummy());
    const auto n = static_cast<std::uint32_t>(platform.monitors().size());
    EXPECT_THROW((void)platform.create_window({"test", n, true, 0, 0}), std::runtime_error);
}

TEST(Platform, NoEventsMeansNoQuit) {
    Platform platform(dummy());
    EXPECT_FALSE(platform.poll_events().quit);
    EXPECT_FALSE(platform.wait_events(1).quit);
}

}  // namespace
}  // namespace fovea
