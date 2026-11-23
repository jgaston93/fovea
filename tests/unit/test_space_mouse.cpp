// D-058: a 3D mouse's reports, and its axes as the observer's rates.

#include <array>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/observer.hpp"
#include "fovea/platform/space_mouse.hpp"

namespace fovea {
namespace {

void put(std::vector<std::uint8_t>& r, int v) {
    const auto u = static_cast<std::uint16_t>(static_cast<std::int16_t>(v));
    r.push_back(static_cast<std::uint8_t>(u & 0xffU));
    r.push_back(static_cast<std::uint8_t>(u >> 8U));
}

std::vector<std::uint8_t> report(std::uint8_t id, std::initializer_list<int> values) {
    std::vector<std::uint8_t> r{id};
    for (const int v : values) {
        put(r, v);
    }
    return r;
}

TEST(SpaceMouse, FoundByVendorAndUsage) {
    EXPECT_TRUE(is_space_mouse(0x256f, 0x01, 0x08));
    EXPECT_TRUE(is_space_mouse(0x046d, 0x01, 0x08));
    EXPECT_FALSE(is_space_mouse(0x256f, 0x01, 0x02));  // its mouse-like collection, a CadMouse
    EXPECT_FALSE(is_space_mouse(0x256f, 0x0c, 0x08));
    EXPECT_FALSE(is_space_mouse(0x045e, 0x01, 0x08));  // another vendor's
}

// The older layout: translation in report 1 (7 bytes), rotation in report 2.
TEST(SpaceMouse, SeparateReports) {
    SpaceMouseRaw raw;
    EXPECT_TRUE(apply_space_mouse_report(report(1, {10, -20, 350}), raw));
    EXPECT_TRUE(apply_space_mouse_report(report(2, {-350, 5, 300}), raw));
    EXPECT_EQ(raw.axes, (std::array<int, 6>{10, -20, 350, -350, 5, 300}));
    // A new translation leaves the rotation as it was.
    EXPECT_TRUE(apply_space_mouse_report(report(1, {0, 0, 0}), raw));
    EXPECT_EQ(raw.axes, (std::array<int, 6>{0, 0, 0, -350, 5, 300}));
}

// The newer: all six in report 1 (13 bytes).
TEST(SpaceMouse, OneReport) {
    SpaceMouseRaw raw;
    EXPECT_TRUE(apply_space_mouse_report(report(1, {1, 2, 3, -4, -5, -32768}), raw));
    EXPECT_EQ(raw.axes, (std::array<int, 6>{1, 2, 3, -4, -5, -32768}));
}

TEST(SpaceMouse, Buttons) {
    SpaceMouseRaw raw;
    EXPECT_TRUE(apply_space_mouse_report({{3, 0x01, 0x00}}, raw));  // a two-button device's left
    EXPECT_EQ(raw.buttons, 1U);
    EXPECT_TRUE(apply_space_mouse_report({{3, 0x00, 0x10, 0x40, 0x02}}, raw));  // a Pro's
    EXPECT_EQ(raw.buttons, 0x02401000U);
    EXPECT_TRUE(apply_space_mouse_report({{3, 0x02}}, raw));  // one byte of mask
    EXPECT_EQ(raw.buttons, 2U);
}

TEST(SpaceMouse, OthersAndShortOnesLeaveItAlone) {
    SpaceMouseRaw raw;
    raw.axes = {1, 2, 3, 4, 5, 6};
    raw.buttons = 7;
    const SpaceMouseRaw before = raw;
    EXPECT_FALSE(apply_space_mouse_report({}, raw));
    EXPECT_FALSE(apply_space_mouse_report({{1, 0, 0, 0, 0, 0}}, raw));  // 6 bytes
    EXPECT_FALSE(apply_space_mouse_report({{2, 0, 0}}, raw));
    EXPECT_FALSE(apply_space_mouse_report({{3}}, raw));
    EXPECT_FALSE(apply_space_mouse_report({{23, 80}}, raw));  // the battery's
    EXPECT_EQ(raw.axes, before.axes);
    EXPECT_EQ(raw.buttons, before.buttons);
}

TEST(SpaceMouse, StateScalesAndFindsPresses) {
    SpaceMouseRaw raw;
    raw.axes = {175, -350, 700, 0, -1000, 35};
    raw.buttons = 0b011;
    const SpaceMouseState s = space_mouse_state(raw, 0b010);
    EXPECT_TRUE(s.connected);
    EXPECT_DOUBLE_EQ(s.axes[0], 0.5);
    EXPECT_DOUBLE_EQ(s.axes[1], -1.0);
    EXPECT_DOUBLE_EQ(s.axes[2], 1.0);  // past full scale: clamped
    EXPECT_DOUBLE_EQ(s.axes[4], -1.0);
    EXPECT_DOUBLE_EQ(s.axes[5], 0.1);
    EXPECT_EQ(s.buttons, 0b011U);
    EXPECT_EQ(s.pressed, 0b001U);  // only the one that went down
}

// The cap as if holding the entity: twisted clockwise the camera's bearing
// goes down; tilted toward the user, higher; pulled toward the user, nearer.
TEST(SpaceMouse, AxesAsTheObserversRates) {
    const auto rates = [](std::size_t i, double v) {
        std::array<double, 6> a{};
        a[i] = v;
        return observer_rates_6dof(a);
    };
    EXPECT_DOUBLE_EQ(rates(5, 1.0).azimuth_dps, -kSpaceMouseAzimuthDps);
    EXPECT_DOUBLE_EQ(rates(3, 1.0).elevation_dps, kSpaceMouseElevationDps);
    EXPECT_DOUBLE_EQ(rates(1, 1.0).zoom_per_s, -kSpaceMouseZoomPerS);
    EXPECT_DOUBLE_EQ(rates(1, -1.0).zoom_per_s, kSpaceMouseZoomPerS);
    // Inside the dead zone, nothing; past it, a square law from 0.
    EXPECT_EQ(rates(5, kSpaceMouseDeadZone).azimuth_dps, 0.0);
    const double half = kSpaceMouseDeadZone + 0.5 * (1.0 - kSpaceMouseDeadZone);
    EXPECT_NEAR(rates(5, -half).azimuth_dps, 0.25 * kSpaceMouseAzimuthDps, 1e-9);
    // The others do nothing.
    for (const std::size_t i : {std::size_t{0}, std::size_t{2}, std::size_t{4}}) {
        const ObserverRates r = rates(i, 1.0);
        EXPECT_EQ(r.azimuth_dps, 0.0);
        EXPECT_EQ(r.elevation_dps, 0.0);
        EXPECT_EQ(r.zoom_per_s, 0.0);
    }
}

// Looked for where there may be none (CI's machines): harmless, and with
// none, still.
TEST(SpaceMouse, NoneIsFine) {
    SpaceMouse m;
    for (int i = 0; i < 3; ++i) {
        const SpaceMouseState s = m.poll();
        if (!s.connected) {
            EXPECT_EQ(s.axes, (std::array<double, 6>{}));
            EXPECT_EQ(s.pressed, 0U);
        }
    }
}

TEST(SpaceMouse, NextModeCycles) {
    ObserverControls c{ObserverConfig{}};
    c.request_next_mode();
    EXPECT_EQ(c.advance(0.0).mode, ObserverMode::tether);
    c.request_next_mode();
    EXPECT_EQ(c.advance(0.0).mode, ObserverMode::top_down);
    c.request_next_mode();
    EXPECT_EQ(c.advance(0.0).mode, ObserverMode::orbit);
}

}  // namespace
}  // namespace fovea
