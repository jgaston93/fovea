// Particle volumes drawn (D-040): a cloud in front of the reference scene's
// eye shows, tinted by its albedo and lit by the sky; one buried in the
// ground doesn't; the eye inside one sees it everywhere; a faded or empty
// frame draws nothing; drawing them allocates nothing. With
// FOVEA_PARTICLE_PNG set to a directory, the images are written there to
// look at.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/geodesy.hpp"
#include "fovea/core/image_file.hpp"
#include "fovea/core/particles.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/render/view.hpp"
#include "support/alloc_counter.hpp"

namespace fovea {
namespace {

using namespace std::chrono_literals;
constexpr std::uint32_t kW = 160;
constexpr std::uint32_t kH = 90;

std::unique_ptr<Renderer> make_renderer(std::size_t capacity) {
    RendererOptions options;
    options.particle_capacity = capacity;
    options.msaa = 1;
    if (std::getenv("FOVEA_REQUIRE_VALIDATION") != nullptr) {
        options.validation = Validation::required;
    }
    try {
        return Renderer::offscreen(std::vector<OffscreenOutput>{{kW, kH}}, options);
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

// The reference scene's one view: level, facing north, 2 m up.
SceneView eye_view() {
    ChannelConfig ch;
    ch.name = "test";
    ch.render_width = kW;
    ch.render_height = kH;
    ch.frustum = {-30.0, 30.0, 17.0, -17.0};
    const ViewDesc v = view_from_channel(ch, DisplayConfig{}, 0);
    return reference_scene_view(v, ReferenceScene{});
}

// A wide, lopsided channel: from 30 to 86 degrees right of its axis.
SceneView wide_view() {
    ChannelConfig ch;
    ch.name = "wide";
    ch.render_width = kW;
    ch.render_height = kH;
    ch.frustum = {30.0, 86.0, 17.0, -17.0};
    const ViewDesc v = view_from_channel(ch, DisplayConfig{}, 0);
    return reference_scene_view(v, ReferenceScene{});
}

// A frame with one origin on the ground under the eye (east, north, up).
ParticleFrame frame_at_eye(const SceneView& view, std::size_t capacity) {
    ParticleFrame f;
    f.particles.resize(capacity);
    const Geodetic eye = ecef_to_geodetic(view.eye_ecef);
    const Geodetic ground{eye.lat_deg, eye.lon_deg,
                          eye.alt_m - static_cast<double>(view.eye_height_m)};
    ParticleOrigin o;
    o.ecef = geodetic_to_ecef(ground);
    const DMat3 ned = ned_to_ecef(ground.lat_deg, ground.lon_deg);
    o.enu_to_ecef = DMat3(ned[1], ned[0], -ned[2]);
    f.origins.push_back(o);
    return f;
}

void add(ParticleFrame& f, LocalTime t, Vec3 enu, float radius, float density) {
    Particle& p = f.particles.at(f.count++);
    p.position = enu;
    p.radius = radius;
    p.bulk_density = density;
    p.received = t;
    p.first_seen = t - 10s;  // faded in
    p.fade_from = t + 10s;
}

std::vector<std::uint8_t> draw(Renderer& r, const SceneView& view, const ParticleFrame* f,
                               LocalTime t, const char* png) {
    r.set_particles(f, t);
    r.draw_scene_frame(std::span(&view, 1));
    auto px = r.read_back_rgba(0);
    if (const char* dir = std::getenv("FOVEA_PARTICLE_PNG"); dir != nullptr && png != nullptr) {
        (void)write_png(std::filesystem::path(dir) / png, RgbaImage{kW, kH, px});
    }
    return px;
}

struct Rgb {
    double r = 0, g = 0, b = 0;
};

// The mean colour of a block of pixels, 0 to 255.
Rgb mean(const std::vector<std::uint8_t>& px, std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
         std::uint32_t y1) {
    Rgb m;
    double n = 0;
    for (std::uint32_t y = y0; y < y1; ++y) {
        for (std::uint32_t x = x0; x < x1; ++x) {
            const std::size_t i = (std::size_t{y} * kW + x) * 4;
            m.r += px[i];
            m.g += px[i + 1];
            m.b += px[i + 2];
            n += 1;
        }
    }
    return {m.r / n, m.g / n, m.b / n};
}

double difference(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    double d = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        d += std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
    }
    return d / static_cast<double>(a.size());
}

// A low wall of dust 60 to 90 m ahead, across the view, up to about 5 m
// (under 3 degrees above the eye: the view's top rows stay sky).
void add_wall(ParticleFrame& f, LocalTime t) {
    for (int i = 0; i < 16; ++i) {
        for (int j = 0; j < 4; ++j) {
            for (int k = 0; k < 2; ++k) {
                add(f, t,
                    Vec3(-60.0F + 8.0F * static_cast<float>(i),
                         60.0F + 10.0F * static_cast<float>(j),
                         1.0F + 2.5F * static_cast<float>(k)),
                    3.0F, 5e-4F);  // 0.25 per metre at the default 500 m^2/kg
            }
        }
    }
}

TEST(ParticlesRender, ADustWallShowsInFrontAndLeavesTheSkyAboveIt) {
    auto r = make_renderer(1000);
    FOVEA_NEED_VULKAN(r);
    const SceneView view = eye_view();
    const LocalTime t = now();
    const auto clear = draw(*r, view, nullptr, t, "particles-none.png");
    ParticleFrame f = frame_at_eye(view, 1000);
    add_wall(f, t);
    const auto dusty = draw(*r, view, &f, t, "particles-wall.png");
    EXPECT_EQ(r->validation_messages(), 0U);
    // The wall against the sky just above the horizon (row 45; the wall
    // reaches about 4 degrees, 10 rows, above it), much changed; the top
    // rows, the sky well above the wall, hardly.
    const Rgb c0 = mean(clear, 20, 38, 140, 44);
    const Rgb c1 = mean(dusty, 20, 38, 140, 44);
    const double change = std::abs(c1.r - c0.r) + std::abs(c1.g - c0.g) + std::abs(c1.b - c0.b);
    EXPECT_GT(change, 30.0);
    const Rgb s0 = mean(clear, 0, 0, kW, 5);
    const Rgb s1 = mean(dusty, 0, 0, kW, 5);
    EXPECT_LT(std::abs(s1.r - s0.r) + std::abs(s1.g - s0.g) + std::abs(s1.b - s0.b), 6.0);
    // Dust-coloured: redder than blue where it is; lit as the scene is,
    // not blown out (the view's exposure applied).
    EXPECT_GT(c1.r, c1.b);
    EXPECT_LT(c1.r, 240.0);
    EXPECT_GT(c1.g, 60.0);
}

TEST(ParticlesRender, DustUnderTheGroundIsntDrawn) {
    auto r = make_renderer(10);
    FOVEA_NEED_VULKAN(r);
    const SceneView view = eye_view();
    const LocalTime t = now();
    const auto clear = draw(*r, view, nullptr, t, nullptr);
    ParticleFrame f = frame_at_eye(view, 10);
    // In the middle of the view's lower half (about 3.5 to 13.5 degrees
    // down), its top half a metre below the ground.
    add(f, t, Vec3(0.0F, 40.0F, -4.0F), 3.5F, 5e-3F);
    const auto buried = draw(*r, view, &f, t, "particles-buried.png");
    EXPECT_LT(difference(clear, buried), 0.5);
    EXPECT_EQ(r->validation_messages(), 0U);
}

// The eye inside a puff: it covers the whole view, nearly evenly.
TEST(ParticlesRender, TheEyeInsideAPuffSeesItEverywhere) {
    auto r = make_renderer(10);
    FOVEA_NEED_VULKAN(r);
    const SceneView view = eye_view();
    const LocalTime t = now();
    const auto clear = draw(*r, view, nullptr, t, nullptr);
    ParticleFrame f = frame_at_eye(view, 10);
    add(f, t, Vec3(0.0F, 0.0F, 2.0F), 40.0F, 2e-3F);  // 1 per metre, the eye at its centre
    const auto inside = draw(*r, view, &f, t, "particles-inside.png");
    EXPECT_EQ(r->validation_messages(), 0U);
    // Every corner changed, the sky's included.
    for (const auto& [x, y] : {std::pair{0U, 0U}, std::pair{kW - 8, 0U}, std::pair{0U, kH - 8},
                               std::pair{kW - 8, kH - 8}}) {
        const Rgb a = mean(clear, x, y, x + 8, y + 8);
        const Rgb b = mean(inside, x, y, x + 8, y + 8);
        EXPECT_GT(std::abs(b.r - a.r) + std::abs(b.g - a.g) + std::abs(b.b - a.b), 20.0)
            << x << "," << y;
    }
    // Ahead, the ground is hidden: the top and bottom halves look alike.
    const Rgb top = mean(inside, 0, 0, kW, kH / 2);
    const Rgb bottom = mean(inside, 0, kH / 2, kW, kH);
    EXPECT_LT(std::abs(top.r - bottom.r) + std::abs(top.g - bottom.g) + std::abs(top.b - bottom.b),
              30.0);
}

// The eye inside a thin puff, near its edge: ahead, the ray is inside it
// for 5 m, from the eye (not from where the line through it would enter, 75
// m behind), so the scene shows through.
TEST(ParticlesRender, ThinDustAroundTheEyeIsSeenThrough) {
    auto r = make_renderer(10);
    FOVEA_NEED_VULKAN(r);
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleFrame f = frame_at_eye(view, 10);
    add(f, t, Vec3(0.0F, -35.0F, 2.0F), 40.0F, 1e-4F);  // 0.05 per metre
    const auto thin = draw(*r, view, &f, t, "particles-thin.png");
    EXPECT_EQ(r->validation_messages(), 0U);
    // The sky and the ground still differ.
    const Rgb sky = mean(thin, 0, 0, kW, 20);
    const Rgb ground = mean(thin, 0, kH - 20, kW, kH);
    EXPECT_GT(std::abs(sky.b - ground.b) + std::abs(sky.g - ground.g), 40.0);
}

// The full-view quad reaches the edges of any channel the config allows
// (edges below 90 degrees): every column of a wide, lopsided one changes.
TEST(ParticlesRender, TheEyeInsideAPuffFillsAWideChannel) {
    auto r = make_renderer(10);
    FOVEA_NEED_VULKAN(r);
    const SceneView view = wide_view();
    const LocalTime t = now();
    const auto clear = draw(*r, view, nullptr, t, nullptr);
    ParticleFrame f = frame_at_eye(view, 10);
    add(f, t, Vec3(0.0F, 0.0F, 2.0F), 40.0F, 2e-3F);
    const auto inside = draw(*r, view, &f, t, "particles-wide.png");
    EXPECT_EQ(r->validation_messages(), 0U);
    for (std::uint32_t x = 0; x < kW; x += 8) {
        const Rgb a = mean(clear, x, 0, x + 8, kH);
        const Rgb b = mean(inside, x, 0, x + 8, kH);
        EXPECT_GT(std::abs(b.r - a.r) + std::abs(b.g - a.g) + std::abs(b.b - a.b), 20.0) << x;
    }
}

TEST(ParticlesRender, FadedOrEmptyDrawsNothing) {
    auto r = make_renderer(1000);
    FOVEA_NEED_VULKAN(r);
    const SceneView view = eye_view();
    const LocalTime t = now();
    const auto clear = draw(*r, view, nullptr, t, nullptr);
    ParticleFrame f = frame_at_eye(view, 1000);
    add_wall(f, t);
    for (std::size_t i = 0; i < f.count; ++i) {
        f.particles[i].fade_from = t - 10s;  // long faded out
    }
    EXPECT_LT(difference(clear, draw(*r, view, &f, t, nullptr)), 0.5);
    ParticleFrame empty = frame_at_eye(view, 1000);
    EXPECT_LT(difference(clear, draw(*r, view, &empty, t, nullptr)), 0.5);
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(ParticlesRender, DrawingThemAllocatesNothing) {
    RendererOptions options;
    options.particle_capacity = 1000;
    options.msaa = 1;
    options.validation = Validation::off;  // the layer allocates on the calling thread
    std::unique_ptr<Renderer> r;
    try {
        r = Renderer::offscreen(std::vector<OffscreenOutput>{{kW, kH}}, options);
    } catch (const std::runtime_error&) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    const SceneView view = eye_view();
    LocalTime t = now();
    ParticleFrame f = frame_at_eye(view, 1000);
    add_wall(f, t);
    for (int i = 0; i < 3; ++i) {  // past the first frames' one-time work
        r->set_particles(&f, t);
        r->draw_scene_frame(std::span(&view, 1));
    }
    const test::AllocationCounter counter;
    for (int i = 0; i < 5; ++i) {
        t += 16ms;
        r->set_particles(&f, t);
        r->draw_scene_frame(std::span(&view, 1));
    }
    EXPECT_EQ(counter.count(), 0U);
}

}  // namespace
}  // namespace fovea
