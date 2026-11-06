#include <vector>

#include <gtest/gtest.h>

#include "fovea/platform/monitors.hpp"

namespace fovea {
namespace {

DisplayConfig config_on(std::vector<int> monitors) {
    DisplayConfig c;
    char name = 'a';
    for (int m : monitors) {
        ChannelConfig ch;
        ch.name = std::string(1, name++);
        ch.monitor = m;
        c.channels.push_back(ch);
    }
    return c;
}

const std::vector<MonitorInfo> kThree(3);

TEST(Monitors, ConfigThatFitsHasNoProblems) {
    EXPECT_TRUE(check_monitors(config_on({0, 1, 2}), kThree).empty());
}

TEST(Monitors, MissingMonitorIsReported) {
    const auto problems = check_monitors(config_on({0, 1, 3}), kThree);
    ASSERT_EQ(problems.size(), 1U);
    EXPECT_EQ(problems[0], "channel \"c\" uses monitor 3, but there are only 3 (0 to 2)");
}

TEST(Monitors, SharedMonitorIsReported) {
    const auto problems = check_monitors(config_on({0, 1, 1}), kThree);
    ASSERT_EQ(problems.size(), 1U);
    EXPECT_EQ(problems[0], "channels \"b\" and \"c\" both use monitor 1");
}

TEST(Monitors, OnlyMultiSwapchainIsChecked) {
    DisplayConfig spanned = config_on({0, 0, 0});
    spanned.presenter = PresenterKind::spanned;
    EXPECT_TRUE(check_monitors(spanned, std::vector<MonitorInfo>(1)).empty());
    DisplayConfig offscreen = config_on({0, 1, 2});
    offscreen.presenter = PresenterKind::offscreen;
    EXPECT_TRUE(check_monitors(offscreen, {}).empty());  // a headless machine
}

TEST(Monitors, NoMonitorsAtAll) {
    const auto problems = check_monitors(config_on({0}), {});
    ASSERT_EQ(problems.size(), 1U);
    EXPECT_EQ(problems[0], "channel \"a\" uses monitor 0, but there are only 0 (0 to -1)");
}

}  // namespace
}  // namespace fovea
