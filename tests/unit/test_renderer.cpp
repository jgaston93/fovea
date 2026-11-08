#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/math.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/render/view.hpp"

namespace fovea {
namespace {

// Offscreen rendering needs a Vulkan 1.3 driver but no display: CI runs it on
// lavapipe (Mesa's CPU driver). Machines without any driver skip, unless
// FOVEA_REQUIRE_VULKAN is set (as in CI), where a missing driver fails.
// FOVEA_REQUIRE_VALIDATION (CI, with the layer installed) makes a missing
// validation layer a failure too.
std::unique_ptr<Renderer> make_offscreen(std::vector<OffscreenOutput> outputs) {
    RendererOptions options;
    if (std::getenv("FOVEA_REQUIRE_VALIDATION") != nullptr) {
        options.validation = Validation::required;
    }
    try {
        return Renderer::offscreen(outputs, options);
    } catch (const std::runtime_error& e) {
        if (std::getenv("FOVEA_REQUIRE_VULKAN") != nullptr) {
            ADD_FAILURE() << "no Vulkan: " << e.what();
        }
        return nullptr;
    }
}

#define FOVEA_NEED_VULKAN(renderer)                             \
    if ((renderer) == nullptr) {                                \
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine"; \
    }

TEST(Renderer, OffscreenClearReadsBackItsColour) {
    auto r = make_offscreen({{64, 32}, {16, 16}});
    FOVEA_NEED_VULKAN(r);
    EXPECT_EQ(r->output_count(), 2U);
    r->draw_clear_frame(std::vector<Rgb8>{{200, 30, 30}, {30, 170, 60}});
    const auto a = r->read_back_rgba(0);
    const auto b = r->read_back_rgba(1);
    ASSERT_EQ(a.size(), 64U * 32U * 4U);
    ASSERT_EQ(b.size(), 16U * 16U * 4U);
    // Every pixel, not just one: the whole image must be cleared.
    int wrong = 0;
    for (std::size_t i = 0; i < a.size(); i += 4) {
        wrong += (a[i] != 200 || a[i + 1] != 30 || a[i + 2] != 30 || a[i + 3] != 255) ? 1 : 0;
    }
    for (std::size_t i = 0; i < b.size(); i += 4) {
        wrong += (b[i] != 30 || b[i + 1] != 170 || b[i + 2] != 60 || b[i + 3] != 255) ? 1 : 0;
    }
    EXPECT_EQ(wrong, 0);
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(Renderer, LaterFramesReplaceEarlierOnes) {
    auto r = make_offscreen({{8, 8}});
    FOVEA_NEED_VULKAN(r);
    for (const std::uint8_t v : std::vector<std::uint8_t>{10, 20, 30, 40, 50}) {
        r->draw_clear_frame(std::vector<Rgb8>{{v, v, v}});
    }
    const auto px = r->read_back_rgba(0);
    EXPECT_EQ(px[0], 50);
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(Renderer, ReportsTheGpu) {
    auto r = make_offscreen({{4, 4}});
    FOVEA_NEED_VULKAN(r);
    EXPECT_FALSE(r->gpu().name.empty());
    EXPECT_GE(r->gpu().api_major * 10 + r->gpu().api_minor, 13U);
}

TEST(Renderer, OnlyOneAtATime) {
    auto first = make_offscreen({{4, 4}});
    FOVEA_NEED_VULKAN(first);
    EXPECT_THROW((void)Renderer::offscreen(std::vector<OffscreenOutput>{{4, 4}}),
                 std::runtime_error);
    first.reset();
    EXPECT_NE(make_offscreen({{4, 4}}), nullptr);  // free again once the first is gone
}

TEST(Renderer, ReadBackNeedsAFrameAndAnOffscreenOutput) {
    auto r = make_offscreen({{4, 4}});
    FOVEA_NEED_VULKAN(r);
    EXPECT_THROW((void)r->read_back_rgba(0), std::runtime_error);  // nothing drawn yet
    r->draw_clear_frame(std::vector<Rgb8>{{1, 2, 3}});
    EXPECT_THROW((void)r->read_back_rgba(1), std::runtime_error);
}

// --- The reference scene (step 6) ------------------------------------------
// Geometric checks, true on any GPU: where the horizon and grid lines land
// follows from the projection, so the tests compute it independently.

constexpr std::uint32_t kW = 320;
constexpr std::uint32_t kH = 360;  // tall, so grid rows are well apart

struct Rgb {
    int r;
    int g;
    int b;
};

struct Image {
    std::vector<std::uint8_t> px;
    Rgb at(std::uint32_t x, std::uint32_t y) const {
        const std::size_t i = (static_cast<std::size_t>(y) * kW + x) * 4;
        return {px[i], px[i + 1], px[i + 2]};
    }
    bool sky(std::uint32_t x, std::uint32_t y) const {
        const Rgb c = at(x, y);
        // Not the ground (olive: greener than blue) nor the horizon line
        // (gold: redder than blue). The sky, blue overhead, is nearly white
        // at the horizon toward the sun, so isn't told by its blue.
        return c.g <= c.b + 8 && c.r <= c.b + 20;
    }
    int brightness(std::uint32_t x, std::uint32_t y) const {
        const Rgb c = at(x, y);
        return c.r + c.g + c.b;
    }
};

ViewDesc channel_view(Euler rotation, Frustum frustum) {
    ChannelConfig ch;
    ch.name = "test";
    ch.render_width = kW;
    ch.render_height = kH;
    ch.view_rotation = rotation;
    ch.frustum = frustum;
    return view_from_channel(ch, DisplayConfig{}, 0);
}

// Renders one frame of the scene for each view and reads every output back.
std::vector<Image> render(const std::vector<ViewDesc>& views, const ReferenceScene& scene) {
    auto r = make_offscreen(std::vector<OffscreenOutput>(views.size(), {kW, kH}));
    if (r == nullptr) {
        return {};
    }
    std::vector<SceneView> scene_views;
    for (const ViewDesc& v : views) {
        scene_views.push_back(reference_scene_view(v, scene));
    }
    r->draw_scene_frame(scene_views);
    std::vector<Image> images;
    for (std::uint32_t i = 0; i < views.size(); ++i) {
        images.push_back({r->read_back_rgba(i)});
    }
    EXPECT_EQ(r->validation_messages(), 0U);
    return images;
}

// The first row, from the top, that is not sky.
int horizon_row(const Image& img, std::uint32_t x) {
    for (std::uint32_t y = 0; y < kH; ++y) {
        if (!img.sky(x, y)) {
            return static_cast<int>(y);
        }
    }
    return static_cast<int>(kH);
}

// The image row (in pixels, top 0) where a direction at `up_deg` above the
// view axis lands, for a frustum spanning top..bottom degrees.
double row_of(double up_deg, double top_deg, double bottom_deg) {
    const double t = std::tan(deg_to_rad(top_deg));
    const double b = std::tan(deg_to_rad(bottom_deg));
    return (t - std::tan(deg_to_rad(up_deg))) / (t - b) * kH;
}

TEST(ReferenceScene, HorizonIsWhereTheProjectionPutsIt) {
    // An asymmetric frustum: the level horizon is not in the middle.
    const Frustum f{-30.0, 30.0, 25.0, -10.0};
    const auto images = render({channel_view({}, f)}, ReferenceScene{});
    if (images.empty()) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    EXPECT_NEAR(horizon_row(images[0], kW / 2), row_of(0.0, 25.0, -10.0), 2.0);
}

TEST(ReferenceScene, PitchingUpLowersTheHorizon) {
    const Frustum f{-30.0, 30.0, 17.0, -17.0};
    const auto images = render({channel_view({0.0, 10.0, 0.0}, f)}, ReferenceScene{});
    if (images.empty()) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    // Nose up 10 degrees: the horizon is 10 degrees below the view axis.
    EXPECT_NEAR(horizon_row(images[0], kW / 2), row_of(-10.0, 17.0, -17.0), 2.0);
}

TEST(ReferenceScene, RollTiltsTheHorizon) {
    const Frustum f{-30.0, 30.0, 17.0, -17.0};
    const auto images = render({channel_view({0.0, 0.0, 10.0}, f)}, ReferenceScene{});
    if (images.empty()) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    // Right wing down: the horizon rises on the right of the picture.
    const int left = horizon_row(images[0], kW / 8);
    const int right = horizon_row(images[0], kW * 7 / 8);
    EXPECT_GT(left - right, 20);
    EXPECT_NEAR(horizon_row(images[0], kW / 2), kH / 2.0, 2.0);  // pivots about the centre
}

// The bright grid rows in one column, as row positions (centre of each run).
// The bright runs down column x from from_row: each run's first and last
// row. Bright is a third of the way from the ground (the column's darkest)
// to its brightest line: far lines are thin and antialiased, fainter in one
// column than the next.
std::vector<std::pair<double, double>> line_runs(const Image& img, std::uint32_t x,
                                                 std::uint32_t from_row) {
    std::vector<std::pair<double, double>> runs;
    int brightest = 0;
    int darkest = 1 << 30;
    for (std::uint32_t y = from_row; y < kH; ++y) {
        brightest = std::max(brightest, img.brightness(x, y));
        darkest = std::min(darkest, img.brightness(x, y));
    }
    const int threshold = darkest + (brightest - darkest) / 3;
    for (std::uint32_t y = from_row; y < kH;) {
        if (img.brightness(x, y) < threshold) {
            ++y;
            continue;
        }
        const std::uint32_t start = y;
        while (y < kH && img.brightness(x, y) >= threshold) {
            ++y;
        }
        runs.emplace_back(start, y - 1);
    }
    return runs;
}

std::vector<double> line_rows(const Image& img, std::uint32_t x, std::uint32_t from_row) {
    std::vector<double> rows;
    int threshold = 0;
    for (std::uint32_t y = from_row; y < kH; ++y) {
        threshold = std::max(threshold, img.brightness(x, y));
    }
    threshold = threshold * 2 / 3;
    for (std::uint32_t y = from_row; y < kH;) {
        if (img.brightness(x, y) < threshold) {
            ++y;
            continue;
        }
        const std::uint32_t start = y;
        while (y < kH && img.brightness(x, y) >= threshold) {
            ++y;
        }
        rows.push_back((start + y - 1) / 2.0);
    }
    return rows;
}

TEST(ReferenceScene, GridLinesAreAtTheirDistances) {
    // Eye 10 m up, lines every 10 m, looking north: the east-west line k * 10 m
    // ahead is atan(10 / (10 k)) below the horizon.
    ReferenceScene scene;
    scene.eye_height_m = 10.0;
    scene.grid_spacing_m = 10.0;
    const Frustum f{-30.0, 30.0, 17.0, -17.0};
    const auto images = render({channel_view({}, f)}, scene);
    if (images.empty()) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    // A column a little right of centre, clear of the north-south lines
    // there (they converge on the centre).
    const std::uint32_t x = kW / 2 + 3;
    const auto rows = line_rows(images[0], x, kH / 2 + 2);
    for (int k : {4, 5, 6}) {
        const double expected = row_of(-rad_to_deg(std::atan(1.0 / k)), 17.0, -17.0);
        double nearest = 1e9;
        for (double r : rows) {
            nearest = std::abs(r - expected) < std::abs(nearest - expected) ? r : nearest;
        }
        EXPECT_NEAR(nearest, expected, 1.5) << "line " << k * 10 << " m ahead";
    }
}

constexpr double kEdgeTolerancePx = 2.0;

TEST(ReferenceScene, LinesContinueAcrossTheSharedEdge) {
    // Two channels of the example layout, 60 degrees apart, edge to edge: the
    // grid lines leaving the left channel's right edge must enter the centre
    // channel's left edge at the same heights.
    ReferenceScene scene;
    scene.eye_height_m = 10.0;
    const Frustum f{-30.0, 30.0, 17.0, -17.0};
    const auto images = render({channel_view({-60.0, 0.0, 0.0}, f), channel_view({}, f)}, scene);
    if (images.empty()) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    // Where the lines are resolved: nearer the horizon they are thinner than
    // a pixel, antialiased, and one edge column sees a line the next just
    // misses (how faint depends on the exposure, milestone 6).
    const auto left = line_runs(images[0], kW - 1, kH / 2 + 50);
    const auto centre = line_runs(images[1], 0, kH / 2 + 50);
    ASSERT_GE(left.size(), 5U);
    ASSERT_GE(centre.size(), 5U);
    // Match by position both ways, not by count: where two lines cross right
    // at the edge, one column sees two runs and the next sees them merged
    // into one, whose middle lies between the two. So each run's middle is
    // matched to the nearest of the other side's runs, anywhere along it.
    // The two edge columns are one pixel apart in angle, so allow a little
    // for the lines' slope.
    const auto nearest = [](const std::vector<std::pair<double, double>>& runs, double r) {
        double best = 1e9;
        for (const auto& [first, last] : runs) {
            best = std::min(best, r < first ? first - r : (r > last ? r - last : 0.0));
        }
        return best;
    };
    const auto middle = [](const std::pair<double, double>& run) {
        return (run.first + run.second) / 2.0;
    };
    for (const auto& run : left) {
        EXPECT_LE(nearest(centre, middle(run)), kEdgeTolerancePx)
            << "left-edge line at row " << middle(run);
    }
    for (const auto& run : centre) {
        EXPECT_LE(nearest(left, middle(run)), kEdgeTolerancePx)
            << "centre-edge line at row " << middle(run);
    }
    EXPECT_EQ(horizon_row(images[0], kW - 1), horizon_row(images[1], 0));
}

TEST(ReferenceScene, EyeOffsetMovesTheEye) {
    // A channel 8 m above a 2 m eye sees what a 10 m eye sees; one moved 5 m
    // east (half a grid cell) sees the grid shifted.
    auto r = make_offscreen({{kW, kH}, {kW, kH}, {kW, kH}});
    FOVEA_NEED_VULKAN(r);
    const Frustum f{-30.0, 30.0, 17.0, -17.0};
    ReferenceScene low;
    low.eye_height_m = 2.0;
    ReferenceScene high;
    high.eye_height_m = 10.0;
    ViewDesc raised = channel_view({}, f);
    raised.pose.offset_m = {0.0, 0.0, -8.0};
    ViewDesc moved = channel_view({}, f);
    moved.pose.offset_m = {0.0, 5.0, 0.0};
    r->draw_scene_frame(std::vector<SceneView>{reference_scene_view(raised, low),
                                               reference_scene_view(channel_view({}, f), high),
                                               reference_scene_view(moved, high)});
    const auto raised_px = r->read_back_rgba(0);
    const auto high_px = r->read_back_rgba(1);
    const auto moved_px = r->read_back_rgba(2);
    // The same to a level: the two eyes, reached differently, can differ in
    // their last bits, and the sky's haze (milestone 6 step 3) is computed
    // from each view's own eye.
    int worst = 0;
    for (std::size_t i = 0; i < raised_px.size(); ++i) {
        worst = std::max(worst, std::abs(int{raised_px[i]} - int{high_px[i]}));
    }
    std::printf("raised against high: at most %d levels apart\n", worst);
    EXPECT_LE(worst, 1);
    EXPECT_NE(moved_px, high_px);
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(ReferenceScene, RepeatedFramesAreIdentical) {
    auto r = make_offscreen({{kW, kH}});
    FOVEA_NEED_VULKAN(r);
    const SceneView v = reference_scene_view(
        channel_view({20.0, -5.0, 3.0}, {-30.0, 30.0, 17.0, -17.0}), ReferenceScene{});
    r->draw_scene_frame(std::vector<SceneView>{v});
    const auto first = r->read_back_rgba(0);
    for (int i = 0; i < 3; ++i) {
        r->draw_scene_frame(std::vector<SceneView>{v});
    }
    EXPECT_EQ(r->read_back_rgba(0), first);
    EXPECT_EQ(r->validation_messages(), 0U);
}

}  // namespace
}  // namespace fovea
