// The host sets the environment (milestone 6 step 7), end to end: the test
// host's session sends CIGI to Fovea's adapter over loopback, the adapter
// writes the scene state, and a headless frame loop draws it. Changing the
// visibility, then the time, changes the image.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "flight.hpp"
#include "fovea/core/celestial.hpp"
#include "fovea/core/config.hpp"
#include "fovea/core/scene.hpp"
#include "fovea/net/cigi_adapter.hpp"
#include "fovea/net/host_thread.hpp"
#include "fovea/render/frame_loop.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/render/view.hpp"
#include "script.hpp"
#include "session.hpp"

namespace fovea {
namespace {

std::unique_ptr<Renderer> make_offscreen() {
    RendererOptions options;
    if (std::getenv("FOVEA_REQUIRE_VALIDATION") != nullptr) {
        options.validation = Validation::required;
    }
    try {
        return Renderer::offscreen(std::vector<OffscreenOutput>{{64, 64}}, options);
    } catch (const std::runtime_error& e) {
        if (std::getenv("FOVEA_REQUIRE_VULKAN") != nullptr) {
            ADD_FAILURE() << "no Vulkan: " << e.what();
        }
        return nullptr;
    }
}

// The mean difference between two images, in levels of 255.
double difference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (i % 4 != 3) {
            sum += std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i]));
        }
    }
    return sum / static_cast<double>(a.size() / 4 * 3);
}

TEST(HostEnvironment, TheHostsVisibilityAndTimeChangeTheImage) {
    auto r = make_offscreen();
    if (r == nullptr) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    // Config's environment: noon over the reference scene's origin (0, 0),
    // the clock stopped, clear air.
    auto scene = std::make_unique<SceneState>();
    scene->edit().environment.julian_date = julian_date(2026, 3, 20, 12.0 * 3600.0);
    scene->edit().environment.visibility_m = 50000.0;
    scene->publish();

    FrameStartRing frames;
    CigiAdapterOptions ig_options;
    ig_options.address = kLoopback;
    CigiAdapter adapter(ig_options, &frames);
    HostThread network(adapter, *scene);
    network.start();

    ChannelConfig ch;
    ch.render_width = 64;
    ch.render_height = 64;
    ch.frustum = {-30.0, 30.0, 30.0, -30.0};
    const std::vector<SceneView> views = {
        reference_scene_view(view_from_channel(ch, DisplayConfig{}, 0), ReferenceScene{})};
    FrameLoopOptions loop_options;
    loop_options.rate_hz = 60.0;
    loop_options.frame_starts = &frames;
    FrameLoop loop(*r, *scene, views, loop_options);

    test_host::Session session({kLoopback, adapter.port()}, 0, kLoopback);
    test_host::FlightState still;
    still.position = {0.0, 0.0, 100.0};
    // Frames until the adapter has applied `updates` environment packets in
    // all, then a few more so the image has them.
    const auto run_until = [&](std::uint64_t updates) {
        int after = 0;
        for (int i = 0; i < 600 && after < 4; ++i) {
            (void)loop.step();
            session.wait_until(now() + std::chrono::milliseconds(2));
            session.send_frame(still, now());
            after += adapter.stats().environment_updates >= updates ? 1 : 0;
        }
        return after == 4;
    };
    ASSERT_TRUE(run_until(0));
    const auto noon = r->read_back_rgba(0);

    test_host::EnvironmentChange haze;
    haze.atmosphere = cigi::AtmosphereControl{};
    haze.atmosphere->visibility_range_m = 1000.0F;
    session.queue_environment(haze);
    ASSERT_TRUE(run_until(1));
    const auto hazy = r->read_back_rgba(0);

    test_host::EnvironmentChange night;
    night.celestial = cigi::CelestialSphereControl{};
    night.celestial->date_time_valid = true;
    night.celestial->ephemeris_enable = false;
    night.celestial->date = 3202026;  // 20 March 2026
    night.celestial->hour = 0;        // midnight at the origin
    session.queue_environment(night);
    ASSERT_TRUE(run_until(2));
    const auto midnight = r->read_back_rgba(0);
    network.stop();

    const double by_haze = difference(noon, hazy);
    const double by_night = difference(hazy, midnight);
    std::printf("mean change: visibility %.1f levels, time %.1f levels\n", by_haze, by_night);
    EXPECT_GT(by_haze, 20.0) << "the host's visibility drew nothing different";
    EXPECT_GT(by_night, 20.0) << "the host's time drew nothing different";
    const EnvironmentState& e = scene->latest().environment;
    EXPECT_EQ(e.visibility_m, 1000.0);
    EXPECT_DOUBLE_EQ(e.julian_date, julian_date(2026, 3, 20, 0.0));
    EXPECT_EQ(adapter.stats().rejected_packets, 0U);
    EXPECT_EQ(r->validation_messages(), 0U);
}

}  // namespace
}  // namespace fovea
