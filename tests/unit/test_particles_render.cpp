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

#include "fovea/core/celestial.hpp"
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

std::unique_ptr<Renderer> make_renderer(std::size_t capacity, const ParticleQuality& quality = {},
                                        const ParticleLook& look = {}) {
    RendererOptions options;
    options.particle_capacity = capacity;
    options.particle_quality = quality;
    options.particle_look = look;
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
                    3.0F, 5e-4F);  // 0.16 per metre at the default 320 m^2/kg
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

// Half-angle slicing (D-041): a puff with a thick one between it and the sun
// is darker than the same puff alone. The sun near the zenith at noon at
// 0 N 0 E (the reference scene's origin); the blocker 20 m above the puff,
// out of the view (its top is 17 degrees up).
TEST(ParticlesRender, APuffInAnothersShadowIsDarker) {
    auto r = make_renderer(10);
    FOVEA_NEED_VULKAN(r);
    r->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleFrame alone = frame_at_eye(view, 10);
    add(alone, t, Vec3(0.0F, 40.0F, 3.0F), 3.0F, 1e-3F);
    ParticleFrame shadowed = alone;
    add(shadowed, t, Vec3(0.0F, 40.0F, 23.0F), 6.0F, 5e-3F);  // 1.6 per metre: opaque
    // The puff's middle, where it's thick: the centre of the view, a little
    // up (it's 1 m above the eye at 40 m).
    const auto lit = draw(*r, view, &alone, t, "particles-lit.png");
    const auto dark = draw(*r, view, &shadowed, t, "particles-shadowed.png");
    EXPECT_EQ(r->validation_messages(), 0U);
    const Rgb a = mean(lit, kW / 2 - 4, kH / 2 - 6, kW / 2 + 4, kH / 2 + 2);
    const Rgb b = mean(dark, kW / 2 - 4, kH / 2 - 6, kW / 2 + 4, kH / 2 + 2);
    // Measured on lavapipe: 402 lit, 313 shadowed (summed over RGB), the shadowed bluer:
    // what's left of the light there is the sky's, which isn't shadowed.
    EXPECT_LT(b.r + b.g + b.b, 0.85 * (a.r + a.g + a.b));
}

// The dust's cover at each pixel: its difference from the empty scene,
// red and blue.
std::vector<int> cover(const std::vector<std::uint8_t>& px,
                       const std::vector<std::uint8_t>& empty) {
    std::vector<int> c(std::size_t{kW} * kH);
    for (std::size_t i = 0; i < c.size(); ++i) {
        c[i] = std::abs(static_cast<int>(px[i * 4]) - static_cast<int>(empty[i * 4])) +
               std::abs(static_cast<int>(px[i * 4 + 2]) - static_cast<int>(empty[i * 4 + 2]));
    }
    return c;
}

// A puff filling the middle of the view, drawn with and without detail
// (D-043), and the empty scene.
struct DetailShots {
    std::vector<std::uint8_t> plain, detailed, empty;
};
DetailShots detail_shots(const ParticleLook& detailed_look, float density, bool& have_vulkan) {
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleFrame f = frame_at_eye(view, 10);
    add(f, t, Vec3(0.0F, 30.0F, 2.0F), 8.0F, density);
    DetailShots s;
    ParticleLook smooth = detailed_look;
    smooth.detail = 0.0F;
    {
        auto a = make_renderer(10, {}, smooth);  // one renderer at a time
        have_vulkan = a != nullptr;
        if (!have_vulkan) {
            return s;
        }
        a->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        s.plain = draw(*a, view, &f, t, "particles-smooth.png");
        s.empty = draw(*a, view, nullptr, t, nullptr);
        EXPECT_EQ(a->validation_messages(), 0U);
    }
    auto b = make_renderer(10, {}, detailed_look);
    b->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
    s.detailed = draw(*b, view, &f, t, "particles-detail.png");
    EXPECT_EQ(b->validation_messages(), 0U);
    return s;
}

// The detail varies the dust's density: inside the puff, its cover changes
// from pixel to pixel far more than a smooth sphere's does.
TEST(ParticlesRender, DetailBreaksUpAPuff) {
    ParticleLook look;
    look.detail = 0.8F;
    look.detail_size = 1.0F;
    bool vulkan = false;
    const DetailShots s = detail_shots(look, 2e-4F, vulkan);  // optical depth 1 through the middle
    if (!vulkan) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    // Over the puff's upper middle (a box within its disc, against the sky:
    // the horizon's edge would swamp it), each pixel's cover
    // against its neighbours' mean: a sphere's changes smoothly.
    const auto roughness = [](const std::vector<int>& c) {
        double sum = 0.0;
        for (std::uint32_t y = kH / 2 - 20; y < kH / 2 - 4; ++y) {  // against the sky
            for (std::uint32_t x = kW / 2 - 12; x < kW / 2 + 12; ++x) {
                const auto at = [&](std::uint32_t u, std::uint32_t v) {
                    return c[std::size_t{v} * kW + u];
                };
                const double around =
                    (at(x - 1, y) + at(x + 1, y) + at(x, y - 1) + at(x, y + 1)) / 4.0;
                sum += std::abs(at(x, y) - around);
            }
        }
        return sum / (16.0 * 24.0);
    };
    const double smooth = roughness(cover(s.plain, s.empty));
    const double rough = roughness(cover(s.detailed, s.empty));
    // Measured on lavapipe: 0.47 smooth, 1.10 detailed.
    EXPECT_GT(rough, 1.8 * smooth) << "roughness " << smooth << " smooth, " << rough << " detailed";
}

// The detail moves the dust about but keeps its mass (D-043): a puff thin
// enough that its cover goes nearly as its optical depth (0.26 through the
// middle), its cover summed over its whole disc, about the same with
// detail as without (measured on lavapipe: 1.5 % less). Without the base's
// normalisation it's 24 % less, without the detail's 42 %.
TEST(ParticlesRender, DetailKeepsTheMass) {
    ParticleLook look;
    look.detail = 0.8F;
    look.detail_size = 1.0F;
    bool vulkan = false;
    const DetailShots s = detail_shots(look, 5e-5F, vulkan);
    if (!vulkan) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    const auto total = [](const std::vector<int>& c) {
        double sum = 0.0;
        for (const int x : c) {
            sum += x;
        }
        return sum;
    };
    const double smooth = total(cover(s.plain, s.empty));
    const double detailed = total(cover(s.detailed, s.empty));
    EXPECT_GT(smooth, 5000.0);
    EXPECT_NEAR(detailed, smooth, 0.06 * smooth)
        << "total cover " << smooth << " smooth, " << detailed << " detailed";
}

// The swirl drifts in time (D-043): the same puff a few seconds on looks
// different with swirl, the same without.
TEST(ParticlesRender, TheSwirlDriftsInTime) {
    ParticleLook still;
    still.swirl = 0.0F;
    still.detail_size = 1.0F;
    ParticleLook swirling = still;
    swirling.swirl = 1.5F;
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleFrame f = frame_at_eye(view, 10);
    add(f, t, Vec3(0.0F, 30.0F, 2.0F), 8.0F, 2e-4F);
    std::vector<std::uint8_t> still_then;
    std::vector<std::uint8_t> still_later;
    {
        auto a = make_renderer(10, {}, still);  // one renderer at a time
        FOVEA_NEED_VULKAN(a);
        still_then = draw(*a, view, &f, t, nullptr);
        still_later = draw(*a, view, &f, t + 4s, nullptr);
        EXPECT_EQ(a->validation_messages(), 0U);
    }
    auto b = make_renderer(10, {}, swirling);
    ASSERT_NE(b, nullptr);
    const auto swirl_then = draw(*b, view, &f, t, "particles-swirl-0.png");
    const auto swirl_later = draw(*b, view, &f, t + 4s, "particles-swirl-4.png");
    EXPECT_EQ(b->validation_messages(), 0U);
    EXPECT_EQ(difference(still_then, still_later), 0.0);
    EXPECT_GT(difference(swirl_then, swirl_later), 0.02) << difference(swirl_then, swirl_later);
}

// An output the frame wasn't given a view for draws no particles: its
// order was never sorted (the renderer draws it with an empty view). It
// checks the output is untouched and validation quiet; the out-of-bounds
// read the guard prevents doesn't show on lavapipe, whose new memory is
// zeros (index 0, drawn off the empty view).
TEST(ParticlesRender, AnOutputWithNoViewDrawsNoParticles) {
    RendererOptions options;
    options.particle_capacity = 1000;
    options.msaa = 1;
    if (std::getenv("FOVEA_REQUIRE_VALIDATION") != nullptr) {
        options.validation = Validation::required;
    }
    std::unique_ptr<Renderer> r;
    try {
        r = Renderer::offscreen(std::vector<OffscreenOutput>{{kW, kH}, {kW, kH}}, options);
    } catch (const std::runtime_error&) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    const SceneView view = eye_view();
    const LocalTime t = now();
    r->set_particles(nullptr, t);
    r->draw_scene_frame(std::span(&view, 1));
    const auto clear = r->read_back_rgba(1);
    ParticleFrame f = frame_at_eye(view, 1000);
    add_wall(f, t);
    for (int i = 0; i < 3; ++i) {  // both frame slots
        r->set_particles(&f, t);
        r->draw_scene_frame(std::span(&view, 1));
    }
    EXPECT_EQ(r->read_back_rgba(1), clear);
    EXPECT_EQ(r->validation_messages(), 0U);
}

// Self-shadowing off (D-042): the same puff, with and without the blocker
// above it, the same.
TEST(ParticlesRender, WithoutSelfShadowingNothingIsShadowed) {
    ParticleQuality q;
    q.self_shadowing = false;
    auto r = make_renderer(10, q);
    FOVEA_NEED_VULKAN(r);
    r->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleFrame alone = frame_at_eye(view, 10);
    add(alone, t, Vec3(0.0F, 40.0F, 3.0F), 3.0F, 1e-3F);
    ParticleFrame shadowed = alone;
    add(shadowed, t, Vec3(0.0F, 40.0F, 23.0F), 6.0F, 5e-3F);
    const auto lit = draw(*r, view, &alone, t, nullptr);
    const auto same = draw(*r, view, &shadowed, t, nullptr);
    EXPECT_EQ(r->validation_messages(), 0U);
    const Rgb a = mean(lit, kW / 2 - 4, kH / 2 - 6, kW / 2 + 4, kH / 2 + 2);
    const Rgb b = mean(same, kW / 2 - 4, kH / 2 - 6, kW / 2 + 4, kH / 2 + 2);
    EXPECT_NEAR(b.r + b.g + b.b, a.r + a.g + a.b, 1.0);
}

// Half resolution (D-042): the wall still drawn, dust-coloured, and dust
// under the ground still hidden.
TEST(ParticlesRender, AtHalfResolutionTheDustIsDrawnAndCut) {
    ParticleQuality q;
    q.half_resolution = true;
    auto r = make_renderer(1000, q);
    FOVEA_NEED_VULKAN(r);
    const SceneView view = eye_view();
    const LocalTime t = now();
    const auto clear = draw(*r, view, nullptr, t, nullptr);
    ParticleFrame f = frame_at_eye(view, 1000);
    add_wall(f, t);
    const auto dusty = draw(*r, view, &f, t, "particles-wall-half.png");
    const Rgb c0 = mean(clear, 20, 38, 140, 44);
    const Rgb c1 = mean(dusty, 20, 38, 140, 44);
    EXPECT_GT(std::abs(c1.r - c0.r) + std::abs(c1.g - c0.g) + std::abs(c1.b - c0.b), 30.0);
    EXPECT_GT(c1.r, c1.b);
    ParticleFrame buried = frame_at_eye(view, 1000);
    add(buried, t, Vec3(0.0F, 40.0F, -4.0F), 3.5F, 5e-3F);
    EXPECT_LT(difference(clear, draw(*r, view, &buried, t, nullptr)), 0.5);
    EXPECT_EQ(r->validation_messages(), 0U);
}

// The particle pass's GPU time is reported (D-042): within the output's,
// and next to nothing without particles.
TEST(ParticlesRender, TheParticlePassIsTimed) {
    auto r = make_renderer(1000);
    FOVEA_NEED_VULKAN(r);
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleFrame f = frame_at_eye(view, 1000);
    add_wall(f, t);
    for (int i = 0; i < 4; ++i) {  // GPU times arrive two frames later
        r->set_particles(&f, t);
        r->draw_scene_frame(std::span(&view, 1));
    }
    const FrameTiming with = r->last_timing();
    if (with.gpu_outputs == 0) {
        // Where a device must be tested (CI, FOVEA_REQUIRE_VULKAN), the
        // timings must come: a timestamp left unwritten loses them all.
        ASSERT_EQ(std::getenv("FOVEA_REQUIRE_VULKAN"), nullptr) << "no GPU timings";
        GTEST_SKIP() << "no GPU timestamps on this device";
    }
    EXPECT_GT(with.particles_ms[0], 0.0F);
    EXPECT_LT(with.particles_ms[0], with.gpu_ms[0]);
    for (int i = 0; i < 4; ++i) {
        r->set_particles(nullptr, t);
        r->draw_scene_frame(std::span(&view, 1));
    }
    const FrameTiming without = r->last_timing();
    EXPECT_LT(without.particles_ms[0], 0.25F * with.particles_ms[0]);
    EXPECT_EQ(r->validation_messages(), 0U);
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

namespace fovea {
namespace {

// Drawn as a volume (D-052): a puff's cover about the same as the puffs'
// (the same optical depth, now through a grid), no validation message, and
// the scene beside it untouched.
TEST(ParticlesVolume, APuffDrawsAsTheSamePuff) {
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleFrame f = frame_at_eye(view, 10);
    add(f, t, Vec3(0.0F, 30.0F, 2.0F), 8.0F, 5e-5F);
    ParticleLook look;
    look.detail = 0.0F;  // the grid alone
    std::vector<std::uint8_t> puffs;
    std::vector<std::uint8_t> volume;
    std::vector<std::uint8_t> empty;
    {
        auto a = make_renderer(10, {}, look);
        if (a == nullptr) {
            GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
        }
        a->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        puffs = draw(*a, view, &f, t, "volume-as-puffs.png");
        empty = draw(*a, view, nullptr, t, nullptr);
    }
    {
        ParticleQuality q;
        q.volume = true;
        q.volume_size = 64;
        q.volume_light_size = 32;
        auto b = make_renderer(10, q, look);
        b->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        volume = draw(*b, view, &f, t, "volume-as-volume.png");
        EXPECT_EQ(b->validation_messages(), 0U);
    }
    const auto total = [](const std::vector<int>& c) {
        double sum = 0.0;
        for (const int x : c) {
            sum += x;
        }
        return sum;
    };
    const double p = total(cover(puffs, empty));
    const double v = total(cover(volume, empty));
    std::printf("total cover: puffs %.0f, volume %.0f\n", p, v);
    EXPECT_GT(p, 5000.0);
    EXPECT_NEAR(v, p, 0.2 * p);
}

}  // namespace
}  // namespace fovea
