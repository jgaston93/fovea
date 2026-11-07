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

#include "fovea/core/attitude.hpp"
#include "fovea/core/geodesy.hpp"
#include "fovea/core/log.hpp"
#include "fovea/core/scene.hpp"
#include "fovea/render/frame_loop.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/render/view.hpp"
#include "support/alloc_counter.hpp"

namespace fovea {
namespace {

// Offscreen outputs: no display, so the loop paces itself.
std::unique_ptr<Renderer> make_offscreen(std::uint32_t outputs, bool validation = true) {
    RendererOptions options;
    if (!validation) {
        options.validation = Validation::off;
    } else if (std::getenv("FOVEA_REQUIRE_VALIDATION") != nullptr) {
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
    // Without the validation layer: it allocates on the calling thread (about
    // 640 times a frame on Linux, where its operator new is ours), and it is
    // a debugging aid, not part of the render path.
    auto r = make_offscreen(2, false);
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

TEST(FrameLoop, OverlayDrawsOnItsOutputOnly) {
    // Two offscreen outputs, the overlay on the first: its top-left corner
    // changes, the second output doesn't, and validation stays quiet.
    auto plain = make_offscreen(2);
    FOVEA_NEED_VULKAN(plain);
    auto scene = std::make_unique<SceneState>();
    const auto v = views(2);
    std::vector<std::uint8_t> plain0;
    std::vector<std::uint8_t> plain1;
    {
        FrameLoop loop(*plain, *scene, v, {1000.0});
        for (int i = 0; i < 3; ++i) {
            (void)loop.step();
        }
        plain0 = plain->read_back_rgba(0);
        plain1 = plain->read_back_rgba(1);
    }
    plain.reset();
    auto with = make_offscreen(2);
    with->enable_overlay(0);
    {
        FrameLoopOptions options{1000.0};
        options.overlay = true;
        FrameLoop loop(*with, *scene, v, options);
        for (int i = 0; i < 3; ++i) {
            (void)loop.step();
        }
    }
    EXPECT_NE(with->read_back_rgba(0), plain0);
    EXPECT_EQ(with->read_back_rgba(1), plain1);
    EXPECT_EQ(with->validation_messages(), 0U);
}

TEST(FrameLoop, SteadyFramesWithTheOverlayAllocateNothing) {
    auto r = make_offscreen(1, false);  // without validation, as above
    FOVEA_NEED_VULKAN(r);
    r->enable_overlay(0);
    // The font is on the GPU before the first frame: drawing never uploads.
    EXPECT_TRUE(r->overlay_ready());
    auto scene = std::make_unique<SceneState>();
    const auto v = views(1);
    FrameLoopOptions options{1000.0};
    options.overlay = true;
    FrameLoop loop(*r, *scene, v, options);
    {
        // A short warm-up allocates (ImGui's first frames build its draw
        // lists; measured, the last allocation is in the fifth frame), which
        // shows the counter sees ImGui's memory: the zero below is real.
        const test::AllocationCounter warm_up;
        for (int i = 0; i < 10; ++i) {
            (void)loop.step();
        }
        EXPECT_GT(warm_up.count(), 0U);
    }
    // Past a full trip round the overlay's 240-frame graph.
    const test::AllocationCounter counter;
    for (int i = 0; i < 300; ++i) {
        (void)loop.step();
    }
    EXPECT_EQ(counter.count(), 0U);
}

TEST(FrameLoop, OverlayNeverUploadsWhileDrawing) {
    // A glyph the font upload in enable_overlay didn't cover (outside
    // printable ASCII) makes ImGui want a texture update. Drawing must not do
    // it (ImGui's backend would block on vkQueueWaitIdle): the overlay is
    // left out of those frames, counted, and the texture stays pending.
    auto r = make_offscreen(1);
    FOVEA_NEED_VULKAN(r);
    r->enable_overlay(0);
    ASSERT_TRUE(r->overlay_ready());
    auto scene = std::make_unique<SceneState>();
    const auto v = views(1);
    FrameLoopOptions options{1000.0};
    options.overlay = true;
    options.overlay_note = "altitude 120\u00b0";  // a degree sign
    FrameLoop loop(*r, *scene, v, options);
    for (int i = 0; i < 5; ++i) {
        (void)loop.step();
    }
    EXPECT_EQ(r->overlay_skipped_frames(), 5U);
    EXPECT_FALSE(r->overlay_ready());  // nobody uploaded it
    EXPECT_EQ(loop.summary().frames, 5U);
    EXPECT_EQ(r->validation_messages(), 0U);
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
    // The last frame has GPU times for two outputs: 16 fixed fields (5 of
    // them terrain) + 2.
    const auto commas = std::count(lines[5].begin(), lines[5].end(), ',');
    EXPECT_EQ(commas, 17);
    EXPECT_EQ(lines[5].rfind("4,", 0), 0U);
}

TEST(FrameLoop, ReportsEachFrameStart) {
    // For the host link's Start of Frame: one record per frame, in order, at
    // the frame's start; when the ring is full the newest are dropped, and
    // the loop doesn't wait.
    auto r = make_offscreen(1);
    FOVEA_NEED_VULKAN(r);
    auto scene = std::make_unique<SceneState>();
    const auto v = views(1);
    FrameStartRing frame_starts;
    FrameLoopOptions options{1000.0};
    options.frame_starts = &frame_starts;
    FrameLoop loop(*r, *scene, v, options);
    const LocalTime before = now();
    for (int i = 0; i < 3; ++i) {
        (void)loop.step();
    }
    for (std::uint64_t i = 0; i < 3; ++i) {
        const auto f = frame_starts.try_pop();
        ASSERT_TRUE(f.has_value());
        EXPECT_EQ(f->frame, i);
        EXPECT_GE(f->t, before);
    }
    EXPECT_FALSE(frame_starts.try_pop().has_value());
    for (int i = 0; i < 70; ++i) {  // more than the ring holds (64)
        (void)loop.step();
    }
    EXPECT_EQ(loop.summary().frames, 73U);
    EXPECT_EQ(frame_starts.try_pop()->frame, 3U);
}

TEST(FrameLoop, ARiggedViewFollowsItsEntity) {
    // The host pitches the entity down, then up: the middle of the image
    // goes from ground to sky (both away from the horizon line).
    auto r = make_offscreen(1);
    FOVEA_NEED_VULKAN(r);
    ChannelConfig ch;
    ch.render_width = 64;
    ch.render_height = 64;
    ch.frustum = {-30.0, 30.0, 17.0, -17.0};
    const std::vector<ViewDesc> descs = {view_from_channel(ch, DisplayConfig{}, 0)};
    const ViewRig rig{descs, ReferenceScene{}, 0.0};
    auto scene = std::make_unique<SceneState>();
    const auto v = views(1);
    FrameLoopOptions options{1000.0};
    options.rig = &rig;
    FrameLoop loop(*r, *scene, v, options);

    auto middle_after = [&](double pitch_deg) {
        SceneSnapshot& s = scene->edit();
        s.entities.clear();
        EntityState e;
        const Geodetic at{0.0, 0.0, 50.0};
        e.cur.ecef = geodetic_to_ecef(at);
        e.cur.orientation = body_to_ecef(at, Euler{0.0, pitch_deg, 0.0});
        (void)s.entities.push_back(e);
        scene->publish();
        (void)loop.step();
        const auto px = r->read_back_rgba(0);
        const std::size_t mid = (32U * 64U + 32U) * 4U;
        return std::array<int, 3>{px[mid], px[mid + 1], px[mid + 2]};
    };
    const auto down = middle_after(-45.0);
    const auto up = middle_after(45.0);
    EXPECT_LT(down[2], down[1]) << "ground: less blue than green";
    EXPECT_GT(up[2], up[1]) << "sky: more blue than green";
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(FrameLoop, ARigMustMatchTheViews) {
    auto r = make_offscreen(1);
    FOVEA_NEED_VULKAN(r);
    auto scene = std::make_unique<SceneState>();
    const auto v = views(1);
    const std::vector<ViewDesc> two(2);
    const ViewRig rig{two, ReferenceScene{}, 0.0};
    FrameLoopOptions options{1000.0};
    options.rig = &rig;
    EXPECT_THROW(FrameLoop(*r, *scene, v, options), std::invalid_argument);
}

TEST(FrameLoop, RunsOnItsOwnThreadAndSeesNewScenes) {
    auto r = make_offscreen(1);
    FOVEA_NEED_VULKAN(r);
    auto scene = std::make_unique<SceneState>();
    const auto v = views(1);
    FrameLoop loop(*r, *scene, v, {120.0});
    loop.start();
    // Publishes while the loop draws, until it has drawn 5 frames: counted,
    // not timed, since a sanitizer build on a small CI runner can draw only a
    // few frames in 200 ms. The deadline only stops a stuck loop.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    for (int i = 0; loop.frames_completed() < 5 && std::chrono::steady_clock::now() < deadline;
         ++i) {
        scene->edit().environment.time_of_day_h = i % 24;
        scene->publish();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    loop.stop();
    EXPECT_GE(loop.summary().frames, 5U);
    EXPECT_EQ(loop.summary().frames, loop.frames_completed());
    EXPECT_EQ(r->validation_messages(), 0U);
}

}  // namespace
}  // namespace fovea
