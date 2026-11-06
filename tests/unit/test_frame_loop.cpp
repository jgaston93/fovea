#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/log.hpp"
#include "fovea/core/scene.hpp"
#include "fovea/render/frame_loop.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/render/view.hpp"
#include "support/alloc_counter.hpp"

namespace fovea {
namespace {

// Offscreen outputs: no display, so the loop paces itself.
std::unique_ptr<Renderer> make_offscreen(std::uint32_t outputs) {
    RendererOptions options;
    if (std::getenv("FOVEA_REQUIRE_VALIDATION") != nullptr) {
        options.validation = Validation::required;
    }
    try {
        return Renderer::offscreen(std::vector<OffscreenOutput>(outputs, {64, 64}), options);
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

std::vector<SceneView> views(std::uint32_t n) {
    ChannelConfig ch;
    ch.render_width = 64;
    ch.render_height = 64;
    ch.frustum = {-30.0, 30.0, 17.0, -17.0};
    return std::vector<SceneView>(
        n, reference_scene_view(view_from_channel(ch, DisplayConfig{}, 0), ReferenceScene{}));
}

TEST(FrameLoop, SelfPacedLoopKeepsToTheRate) {
    auto r = make_offscreen(1);
    FOVEA_NEED_VULKAN(r);
    EXPECT_FALSE(r->paced_by_display());
    auto scene = std::make_unique<SceneState>();
    const auto v = views(1);
    FrameLoop loop(*r, *scene, v, {60.0});
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 31; ++i) {
        (void)loop.step();
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    // 30 periods at least: the loop never runs ahead of its clock. (Slow
    // builds, sanitizers, can run behind it, so there is no upper bound.)
    EXPECT_GE(elapsed, 30.0 / 60.0 - 0.005);
    EXPECT_GE(loop.summary().interval_mean_ms, 16.0);
    EXPECT_EQ(loop.summary().frames, 31U);
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(FrameLoop, GpuTimesArriveForTheFrameTwoBack) {
    auto r = make_offscreen(2);
    FOVEA_NEED_VULKAN(r);
    auto scene = std::make_unique<SceneState>();
    const auto v = views(2);
    FrameLoop loop(*r, *scene, v, {1000.0});
    FrameRecord last;
    for (int i = 0; i < 5; ++i) {
        last = loop.step();
    }
    ASSERT_EQ(last.gpu_outputs, 2U) << "no GPU timestamps on this device";
    EXPECT_EQ(last.gpu_frame, last.frame - 2);
    EXPECT_GT(last.gpu_ms[0], 0.0F);
    EXPECT_GT(last.gpu_ms[1], 0.0F);
    EXPECT_GT(loop.summary().gpu_samples, 0U);
}

TEST(FrameLoop, GpuFramesUseTheLoopsNumbering) {
    // Frames drawn before the loop (the app draws one first) don't shift the
    // GPU frame numbers in the stats.
    auto r = make_offscreen(1);
    FOVEA_NEED_VULKAN(r);
    const auto v = views(1);
    r->draw_scene_frame(v);
    r->draw_scene_frame(v);
    auto scene = std::make_unique<SceneState>();
    FrameLoop loop(*r, *scene, v, {1000.0});
    FrameRecord last;
    for (int i = 0; i < 5; ++i) {
        last = loop.step();
    }
    ASSERT_GT(last.gpu_outputs, 0U);
    EXPECT_EQ(last.gpu_frame, last.frame - 2);
}

TEST(FrameLoop, ALateFrameCountsAsMissed) {
    auto r = make_offscreen(1);
    FOVEA_NEED_VULKAN(r);
    auto scene = std::make_unique<SceneState>();
    const auto v = views(1);
    FrameLoop loop(*r, *scene, v, {60.0});
    (void)loop.step();
    (void)loop.step();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));  // over 1.5 periods late
    const FrameRecord& late = loop.step();
    EXPECT_TRUE(late.missed);
    EXPECT_GE(loop.summary().missed, 1U);
}

TEST(FrameLoop, SteadyFramesAllocateNothing) {
    // The render path rule (architecture.md): after warm-up, a frame
    // allocates nothing on the render thread, stats logging included.
    auto r = make_offscreen(2);
    FOVEA_NEED_VULKAN(r);
    auto scene = std::make_unique<SceneState>();
    Logger stats(256);  // not started: records queue up, then are dropped
    const auto v = views(2);
    FrameLoop loop(*r, *scene, v, {1000.0}, &stats);
    for (int i = 0; i < 10; ++i) {
        (void)loop.step();
    }
    const test::AllocationCounter counter;
    for (int i = 0; i < 100; ++i) {
        (void)loop.step();
    }
    EXPECT_EQ(counter.count(), 0U);
}

TEST(FrameLoop, StatsAreCsvOneLinePerFrame) {
    auto r = make_offscreen(2);
    FOVEA_NEED_VULKAN(r);
    auto scene = std::make_unique<SceneState>();
    std::FILE* f = std::tmpfile();
    ASSERT_NE(f, nullptr);
    {
        Logger stats(256);
        stats.start(make_text_sink(f));
        const auto v = views(2);
        FrameLoop loop(*r, *scene, v, {1000.0}, &stats);
        for (int i = 0; i < 5; ++i) {
            (void)loop.step();
        }
        stats.stop();
    }
    std::rewind(f);
    std::vector<std::string> lines;
    std::array<char, 512> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), f) != nullptr) {
        lines.emplace_back(buf.data());
    }
    std::fclose(f);
    ASSERT_EQ(lines.size(), 6U);  // header and 5 frames
    EXPECT_EQ(lines[0].rfind("frame,t_s,interval_ms", 0), 0U);
    // The last frame has GPU times for two outputs: 8 fixed fields + 2.
    const auto commas = std::count(lines[5].begin(), lines[5].end(), ',');
    EXPECT_EQ(commas, 9);
    EXPECT_EQ(lines[5].rfind("4,", 0), 0U);
}

TEST(FrameLoop, RunsOnItsOwnThreadAndSeesNewScenes) {
    auto r = make_offscreen(1);
    FOVEA_NEED_VULKAN(r);
    auto scene = std::make_unique<SceneState>();
    const auto v = views(1);
    FrameLoop loop(*r, *scene, v, {120.0});
    loop.start();
    for (int i = 0; i < 5; ++i) {
        scene->edit().environment.time_of_day_h = i;
        scene->publish();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    loop.stop();
    EXPECT_GE(loop.summary().frames, 5U);
    EXPECT_EQ(r->validation_messages(), 0U);
}

}  // namespace
}  // namespace fovea
