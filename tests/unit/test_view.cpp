#include <cmath>

#include <gtest/gtest.h>

#include "fovea/core/config.hpp"
#include "fovea/render/view.hpp"

namespace fovea {
namespace {

TEST(View, FromChannelCarriesEveryField) {
    ChannelConfig ch;
    ch.name = "left";
    ch.monitor = 0;
    ch.entity = 17;
    ch.render_width = 1280;
    ch.render_height = 720;
    ch.eye_offset_m = {0.1, -0.2, 0.0};
    ch.view_rotation = {-60.0, 2.0, 0.0};
    ch.frustum = {-30.0, 30.0, 17.0, -17.0};
    DisplayConfig config;
    config.near_m = 0.25;
    const ViewDesc v = view_from_channel(ch, config, 2);
    EXPECT_EQ(v.name, "left");
    EXPECT_EQ(v.pose.kind, PoseSource::Kind::entity);
    EXPECT_EQ(v.pose.entity, 17U);
    EXPECT_EQ(v.pose.offset_m, ch.eye_offset_m);
    EXPECT_EQ(v.pose.rotation.yaw_deg, -60.0);
    EXPECT_EQ(v.pose.rotation.pitch_deg, 2.0);
    EXPECT_EQ(v.projection.frustum.left_deg, -30.0);
    EXPECT_EQ(v.projection.near_m, 0.25);
    EXPECT_EQ(v.target.kind, OutputTarget::Kind::presenter_output);
    EXPECT_EQ(v.target.index, 2U);
    EXPECT_EQ(v.render_width, 1280U);
    EXPECT_EQ(v.render_height, 720U);
}

TEST(View, TargetFollowsThePresenter) {
    DisplayConfig config;
    const ChannelConfig ch;
    config.presenter = PresenterKind::multi_swapchain;
    EXPECT_EQ(view_from_channel(ch, config, 2).target.index, 2U);
    config.presenter = PresenterKind::spanned;
    EXPECT_EQ(view_from_channel(ch, config, 2).target.kind, OutputTarget::Kind::presenter_output);
    EXPECT_EQ(view_from_channel(ch, config, 2).target.index, 0U);  // the one spanned surface
    config.presenter = PresenterKind::offscreen;
    EXPECT_EQ(view_from_channel(ch, config, 2).target.kind, OutputTarget::Kind::offscreen);
    EXPECT_EQ(view_from_channel(ch, config, 2).target.index, 2U);
}

TEST(View, EyeToBodyTurnsTheViewAxis) {
    PoseSource pose;
    pose.rotation = {-60.0, 0.0, 0.0};
    const DVec3 axis = eye_to_body(pose) * DVec3(1.0, 0.0, 0.0);
    EXPECT_NEAR(axis.x, 0.5, 1e-12);                    // cos 60
    EXPECT_NEAR(axis.y, -std::sqrt(3.0) / 2.0, 1e-12);  // to the left
    EXPECT_NEAR(axis.z, 0.0, 1e-12);
}

}  // namespace
}  // namespace fovea
