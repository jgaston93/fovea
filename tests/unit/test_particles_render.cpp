// Particle volumes drawn (D-040): a cloud in front of the reference scene's
// eye shows, tinted by its albedo and lit by the sky; one buried in the
// ground doesn't; the eye inside one sees it everywhere; a faded or empty
// frame draws nothing; drawing them allocates nothing. With
// FOVEA_PARTICLE_PNG set to a directory, the images are written there to
// look at.

#include <algorithm>
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

// The look these tests draw with: the default but for D-040's tan albedo.
// They find the dust by its colour against the scene (cover() sums the red
// and blue differences), and dust's measured albedo (D-054) is nearly
// neutral, close to the sky's and the runway's colour; a distinct tan keeps
// what they measure (where the dust is, how much, its outline) well
// conditioned. The D-054 tests set the albedo they mean.
ParticleLook test_look() {
    ParticleLook tan;
    tan.albedo = {0.62F, 0.52F, 0.40F};
    return tan;
}

std::unique_ptr<Renderer> make_renderer(std::size_t capacity, const ParticleQuality& quality = {},
                                        const ParticleLook& look = test_look()) {
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
    set_puff(p, radius, density);
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
    ParticleLook look = test_look();
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
    ParticleLook look = test_look();
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

// The erosion (D-051) eats into the puff, its mass given back: a puff thin
// enough that its cover goes nearly as its optical depth, its cover summed
// over the disc about the same eroded as not.
TEST(ParticlesRender, ErosionKeepsTheMass) {
    ParticleLook look = test_look();
    look.detail = 0.6F;
    look.detail_size = 1.0F;
    look.erosion = 0.6F;
    bool vulkan = false;
    const DetailShots s = detail_shots(look, 1.5e-5F, vulkan);
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
    const double eroded = total(cover(s.detailed, s.empty));
    std::printf("total cover: smooth %.0f, eroded %.0f\n", smooth, eroded);
    EXPECT_GT(smooth, 1500.0);
    // Measured on lavapipe: 4.6 % less (with 4 samples a chord, 14 %).
    EXPECT_NEAR(eroded, smooth, 0.08 * smooth)
        << "total cover " << smooth << " smooth, " << eroded << " eroded";
}

// The erosion breaks the puff's outline (D-051): a dense puff, its cover
// along a ring at 85 % of its disc's radius varies far more eroded than
// with the detail alone (which leaves the outline a circle).
TEST(ParticlesRender, ErosionBreaksTheOutline) {
    ParticleLook detail_only = test_look();
    detail_only.detail = 0.6F;
    detail_only.detail_size = 1.0F;
    ParticleLook eroding = detail_only;
    eroding.erosion = 0.6F;
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleFrame f = frame_at_eye(view, 10);
    add(f, t, Vec3(0.0F, 30.0F, 2.0F), 8.0F, 2e-3F);
    std::vector<std::uint8_t> plain;
    std::vector<std::uint8_t> eroded;
    std::vector<std::uint8_t> empty;
    {
        auto a = make_renderer(10, {}, detail_only);
        if (a == nullptr) {
            GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
        }
        a->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        plain = draw(*a, view, &f, t, "particles-outline-detail.png");
        empty = draw(*a, view, nullptr, t, nullptr);
    }
    {
        auto b = make_renderer(10, {}, eroding);
        b->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        eroded = draw(*b, view, &f, t, "particles-outline-eroded.png");
        EXPECT_EQ(b->validation_messages(), 0U);
    }
    // The disc: the pixels the uneroded puff covers at all, their centre and
    // radius from their count.
    const std::vector<int> cp = cover(plain, empty);
    const std::vector<int> ce = cover(eroded, empty);
    double cx = 0.0;
    double cy = 0.0;
    double n = 0.0;
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            if (cp[std::size_t{y} * kW + x] > 8) {
                cx += x;
                cy += y;
                n += 1.0;
            }
        }
    }
    ASSERT_GT(n, 400.0);
    cx /= n;
    cy /= n;
    const double radius = std::sqrt(n / 3.14159265);
    const auto spread = [&](const std::vector<int>& c) {
        double sum = 0.0;
        double sum2 = 0.0;
        int k = 0;
        for (int a = 0; a < 180; ++a) {
            const double th = a * 3.14159265 / 90.0;
            const auto x = static_cast<int>(cx + 0.85 * radius * std::cos(th));
            const auto y = static_cast<int>(cy + 0.85 * radius * std::sin(th));
            if (x < 0 || y < 0 || x >= static_cast<int>(kW) || y >= static_cast<int>(kH)) {
                continue;
            }
            const double v = c[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)];
            sum += v;
            sum2 += v * v;
            ++k;
        }
        const double mean = sum / k;
        return std::sqrt(std::max(sum2 / k - mean * mean, 0.0)) / std::max(mean, 1.0);
    };
    const double round = spread(cp);
    const double broken = spread(ce);
    std::printf("the outline's spread along the ring: detail %.3f, eroded %.3f\n", round, broken);
    // Measured with the Gaussian's erosion (D-053, s = 1 - rho^2 / 9: only
    // the outer edge trimmed where the field is high): 0.845 detail alone,
    // 1.134 eroded; with the sphere (until D-053), 0.70 and 1.57.
    EXPECT_GT(broken, 1.2 * round);
}

// The swirl drifts in time (D-043): the same puff a few seconds on looks
// different with swirl, the same without.
TEST(ParticlesRender, TheSwirlDriftsInTime) {
    ParticleLook still = test_look();
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
    // Without: the least of several frames (lavapipe's timestamps are the
    // CPU's, and with other tests running alongside one frame's pass can be
    // held up for milliseconds with nothing in it: CI, 2.9 and 4.7 ms).
    float without_ms = 1e9F;
    for (int i = 0; i < 10; ++i) {
        r->set_particles(nullptr, t);
        r->draw_scene_frame(std::span(&view, 1));
        if (i >= 3) {  // GPU times arrive two frames later
            without_ms = std::min(without_ms, r->last_timing().particles_ms[0]);
        }
    }
    EXPECT_LT(without_ms, 0.25F * with.particles_ms[0]);
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
    ParticleLook look = test_look();
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

// The total and centroid of a cover image.
struct CoverStats {
    double total = 0.0;
    double x = 0.0;
    double y = 0.0;
};
CoverStats cover_stats(const std::vector<int>& c) {
    CoverStats s;
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            const double v = c[std::size_t{y} * kW + x];
            s.total += v;
            s.x += v * x;
            s.y += v * y;
        }
    }
    if (s.total > 0.0) {
        s.x /= s.total;
        s.y /= s.total;
    }
    return s;
}

// Where the volume draws a puff: where the puffs draw it (a ray flipped, or
// the box misplaced, moves it), for two puffs apart on screen.
TEST(ParticlesVolume, PuffsLandWhereThePuffsDo) {
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleLook look = test_look();
    look.detail = 0.0F;
    ParticleQuality q;
    q.volume = true;
    q.volume_size = 96;
    q.volume_light_size = 32;
    for (const Vec3 at : {Vec3(-12.0F, 40.0F, 8.0F), Vec3(10.0F, 35.0F, 4.0F)}) {
        ParticleFrame f = frame_at_eye(view, 10);
        add(f, t, at, 4.0F, 1e-4F);
        std::vector<std::uint8_t> puffs;
        std::vector<std::uint8_t> volume;
        std::vector<std::uint8_t> empty;
        {
            auto a = make_renderer(10, {}, look);
            if (a == nullptr) {
                GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
            }
            a->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
            puffs = draw(*a, view, &f, t, nullptr);
            empty = draw(*a, view, nullptr, t, nullptr);
        }
        {
            auto b = make_renderer(10, q, look);
            b->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
            volume = draw(*b, view, &f, t, nullptr);
            EXPECT_EQ(b->validation_messages(), 0U);
        }
        const CoverStats p = cover_stats(cover(puffs, empty));
        const CoverStats v = cover_stats(cover(volume, empty));
        std::printf("centroid: puffs (%.1f, %.1f), volume (%.1f, %.1f)\n", p.x, p.y, v.x, v.y);
        ASSERT_GT(p.total, 1000.0);
        EXPECT_NEAR(v.x, p.x, 1.5);
        EXPECT_NEAR(v.y, p.y, 1.5);
    }
}

// A puff one or two voxels across keeps its dust as it moves through the
// grid, without flicker (summed at the voxels' centres alone it swung about
// 2x, and moving dust flickered): a far puff makes the voxels 2 m, and a 2.4 m puff steps
// across one diagonally.
TEST(ParticlesVolume, AMovingPuffKeepsItsDust) {
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleLook look = test_look();
    look.detail = 0.0F;
    ParticleQuality q;
    q.volume = true;
    q.volume_size = 128;
    q.volume_light_size = 32;
    std::vector<std::uint8_t> empty;
    std::vector<double> totals;
    {
        auto r = make_renderer(10, q, look);
        if (r == nullptr) {
            GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
        }
        r->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        empty = draw(*r, view, nullptr, t, nullptr);
        for (int k = 0; k < 8; ++k) {
            ParticleFrame f = frame_at_eye(view, 10);
            const auto step = static_cast<float>(k);
            add(f, t, Vec3(0.37F * step, 30.0F + 0.29F * step, 2.0F + 0.23F * step), 2.4F, 2e-4F);
            add(f, t, Vec3(0.0F, 200.0F, -150.0F), 1.0F, 1e-6F);  // behind the eye's view: the box
            const std::vector<std::uint8_t> img = draw(*r, view, &f, t, nullptr);
            totals.push_back(cover_stats(cover(img, empty)).total);
        }
        EXPECT_EQ(r->validation_messages(), 0U);
    }
    const auto [lo, hi] = std::minmax_element(totals.begin(), totals.end());
    // Flicker is the steps' scatter about their trend: a straight line
    // fitted through them. The trend is the puff rising past the eye's
    // height, from in front of the ground to in front of the sky (cover is a
    // colour difference from the empty scene): 2871 rising steadily to 3312
    // on lavapipe since D-054 (dust's light no longer the octaves'); 3148
    // to 3298 before it.
    const auto count = static_cast<double>(totals.size());
    double mx = 0.0;
    double my = 0.0;
    for (std::size_t k = 0; k < totals.size(); ++k) {
        mx += static_cast<double>(k) / count;
        my += totals[k] / count;
    }
    double sxy = 0.0;
    double sxx = 0.0;
    for (std::size_t k = 0; k < totals.size(); ++k) {
        sxy += (static_cast<double>(k) - mx) * (totals[k] - my);
        sxx += (static_cast<double>(k) - mx) * (static_cast<double>(k) - mx);
    }
    double scatter = 0.0;
    for (std::size_t k = 0; k < totals.size(); ++k) {
        const double fit = my + sxy / sxx * (static_cast<double>(k) - mx);
        scatter = std::max(scatter, std::abs(totals[k] - fit) / my);
    }
    std::printf("a moving puff's cover: %.0f to %.0f, %.1f %% off its trend at most\n", *lo, *hi,
                100.0 * scatter);
    ASSERT_GT(*lo, 500.0);
    EXPECT_LT(scatter, 0.03);
    EXPECT_LT(*hi / *lo, 1.2);
}

// The volume's erosion keeps a thin cloud's dust (judged against the
// particles' own density: per metre, thin dust was erased whole).
TEST(ParticlesVolume, ErosionKeepsTheMass) {
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleFrame f = frame_at_eye(view, 10);
    add(f, t, Vec3(0.0F, 30.0F, 2.0F), 8.0F, 5e-5F);
    ParticleQuality q;
    q.volume = true;
    q.volume_size = 96;
    q.volume_light_size = 32;
    ParticleLook plain = test_look();
    plain.detail = 0.6F;
    plain.detail_size = 1.0F;
    ParticleLook eroding = plain;
    eroding.erosion = 0.6F;
    std::vector<std::uint8_t> a_img;
    std::vector<std::uint8_t> b_img;
    std::vector<std::uint8_t> empty;
    {
        auto a = make_renderer(10, q, plain);
        if (a == nullptr) {
            GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
        }
        a->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        a_img = draw(*a, view, &f, t, nullptr);
        empty = draw(*a, view, nullptr, t, nullptr);
    }
    {
        auto b = make_renderer(10, q, eroding);
        b->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        b_img = draw(*b, view, &f, t, nullptr);
        EXPECT_EQ(b->validation_messages(), 0U);
    }
    const double p = cover_stats(cover(a_img, empty)).total;
    const double e = cover_stats(cover(b_img, empty)).total;
    std::printf("a thin cloud's cover: detail %.0f, eroded %.0f\n", p, e);
    ASSERT_GT(p, 5000.0);
    EXPECT_NEAR(e, p, 0.15 * p);
}

// The volume's erosion (D-051, D-052) breaks the cloud's outline: a dense
// puff, its cover along a ring at 85 % of its disc varies far more eroded
// than with the detail alone.
TEST(ParticlesVolume, ErosionBreaksTheCloudsEdge) {
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleFrame f = frame_at_eye(view, 10);
    add(f, t, Vec3(0.0F, 30.0F, 2.0F), 8.0F, 2e-3F);
    ParticleQuality q;
    q.volume = true;
    q.volume_size = 96;
    q.volume_light_size = 32;
    ParticleLook detail_only = test_look();
    detail_only.detail = 0.6F;
    detail_only.detail_size = 0.5F;
    ParticleLook eroding = detail_only;
    eroding.erosion = 0.8F;
    std::vector<std::uint8_t> plain;
    std::vector<std::uint8_t> eroded;
    std::vector<std::uint8_t> empty;
    {
        auto a = make_renderer(10, q, detail_only);
        if (a == nullptr) {
            GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
        }
        a->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        plain = draw(*a, view, &f, t, "volume-edge-detail.png");
        empty = draw(*a, view, nullptr, t, nullptr);
    }
    {
        auto b = make_renderer(10, q, eroding);
        b->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        eroded = draw(*b, view, &f, t, "volume-edge-eroded.png");
        EXPECT_EQ(b->validation_messages(), 0U);
    }
    const std::vector<int> cp = cover(plain, empty);
    const std::vector<int> ce = cover(eroded, empty);
    double cx = 0.0;
    double cy = 0.0;
    double n = 0.0;
    for (std::uint32_t y = 0; y < kH; ++y) {
        for (std::uint32_t x = 0; x < kW; ++x) {
            if (cp[std::size_t{y} * kW + x] > 8) {
                cx += x;
                cy += y;
                n += 1.0;
            }
        }
    }
    ASSERT_GT(n, 400.0);
    cx /= n;
    cy /= n;
    const double radius = std::sqrt(n / 3.14159265);
    const auto spread = [&](const std::vector<int>& c) {
        double sum = 0.0;
        double sum2 = 0.0;
        int k = 0;
        for (int a = 0; a < 180; ++a) {
            const double th = a * 3.14159265 / 90.0;
            const auto x = static_cast<int>(cx + 0.85 * radius * std::cos(th));
            const auto y = static_cast<int>(cy + 0.85 * radius * std::sin(th));
            if (x < 0 || y < 0 || x >= static_cast<int>(kW) || y >= static_cast<int>(kH)) {
                continue;
            }
            const double v = c[static_cast<std::size_t>(y) * kW + static_cast<std::size_t>(x)];
            sum += v;
            sum2 += v * v;
            ++k;
        }
        const double mean = sum / k;
        return std::sqrt(std::max(sum2 / k - mean * mean, 0.0)) / std::max(mean, 1.0);
    };
    const double round = spread(cp);
    const double broken = spread(ce);
    std::printf("the volume's outline along the ring: detail %.3f, eroded %.3f\n", round, broken);
    // Measured on lavapipe: 0.709 detail alone, 0.865 eroded; with the
    // erosion taken out of the march, 0.709. Coarse carving (the field at
    // three times its size, D-052) gives a puff this size few billows round
    // its rim, so the margin is modest.
    EXPECT_GT(broken, 1.12 * round);
}

// A cluster stretched east-west (D-053): seen from the south its cover is
// wider than tall, by its standard deviations' ratio about, in both
// renderers; and the two agree on its total.
TEST(ParticlesGaussian, AStretchedClusterLooksStretched) {
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleFrame f = frame_at_eye(view, 10);
    {
        Particle& p = f.particles.at(f.count++);
        p.position = Vec3(0.0F, 30.0F, 2.0F);
        p.covariance = {16.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F};  // sigma 4 m east, 1 m north and up
        p.extinction_m2 = 8.0F;  // optical depth 0.3 through its middle: thin, so cover tracks it
        p.received = t;
        p.first_seen = t - 10s;
        p.fade_from = t + 10s;
    }
    ParticleLook look = test_look();
    look.detail = 0.0F;
    ParticleQuality vq;
    vq.volume = true;
    vq.volume_size = 96;
    vq.volume_light_size = 32;
    // Its footprint: the pixels over a quarter of its densest's cover.
    const auto extent = [](const std::vector<int>& c, int& w, int& h, double& total) {
        int x0 = 1 << 20;
        int x1 = -1;
        int y0 = 1 << 20;
        int y1 = -1;
        total = 0.0;
        const int most = *std::max_element(c.begin(), c.end());
        for (std::uint32_t y = 0; y < kH; ++y) {
            for (std::uint32_t x = 0; x < kW; ++x) {
                const int v = c[std::size_t{y} * kW + x];
                total += v;
                if (most > 0 && 4 * v > most) {
                    x0 = std::min(x0, static_cast<int>(x));
                    x1 = std::max(x1, static_cast<int>(x));
                    y0 = std::min(y0, static_cast<int>(y));
                    y1 = std::max(y1, static_cast<int>(y));
                }
            }
        }
        w = x1 - x0 + 1;
        h = y1 - y0 + 1;
    };
    std::vector<std::uint8_t> puffs;
    std::vector<std::uint8_t> volume;
    std::vector<std::uint8_t> empty;
    {
        auto a = make_renderer(10, {}, look);
        if (a == nullptr) {
            GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
        }
        a->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        puffs = draw(*a, view, &f, t, "gaussian-stretched-puffs.png");
        empty = draw(*a, view, nullptr, t, nullptr);
        EXPECT_EQ(a->validation_messages(), 0U);
    }
    {
        auto b = make_renderer(10, vq, look);
        b->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        volume = draw(*b, view, &f, t, "gaussian-stretched-volume.png");
        EXPECT_EQ(b->validation_messages(), 0U);
    }
    int pw = 0;
    int ph = 0;
    int vw = 0;
    int vh = 0;
    double ptot = 0.0;
    double vtot = 0.0;
    extent(cover(puffs, empty), pw, ph, ptot);
    extent(cover(volume, empty), vw, vh, vtot);
    std::printf("stretched cluster: puffs %d x %d (cover %.0f), volume %d x %d (cover %.0f)\n", pw,
                ph, ptot, vw, vh, vtot);
    ASSERT_GT(ptot, 1000.0);
    ASSERT_GT(ph, 0);  // found
    ASSERT_GT(vh, 0);
    EXPECT_GT(pw, 2 * ph);
    EXPECT_GT(vw, 2 * vh);
    EXPECT_NEAR(vtot, ptot, 0.15 * ptot);
}

// A cluster tilted 45 degrees in the east-up plane (D-053: its covariance's
// off-diagonal terms): seen from the south, its footprint leans up to the
// right (screen x east, y down) in both renderers. Its cover's second moment
// across x and y negative, its correlation strong.
TEST(ParticlesGaussian, ATiltedClusterLeans) {
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleFrame f = frame_at_eye(view, 10);
    {
        // sigma 4 m along (1, 0, 1) / sqrt 2, 1 m across: I + 15 a a^T.
        Particle& p = f.particles.at(f.count++);
        p.position = Vec3(0.0F, 30.0F, 4.0F);
        p.covariance = {8.5F, 0.0F, 7.5F, 1.0F, 0.0F, 8.5F};
        p.extinction_m2 = 8.0F;
        p.received = t;
        p.first_seen = t - 10s;
        p.fade_from = t + 10s;
    }
    ParticleLook look = test_look();
    look.detail = 0.0F;
    ParticleQuality vq;
    vq.volume = true;
    vq.volume_size = 96;
    vq.volume_light_size = 32;
    const auto lean = [](const std::vector<int>& c) {
        double n = 0.0;
        double mx = 0.0;
        double my = 0.0;
        for (std::uint32_t y = 0; y < kH; ++y) {
            for (std::uint32_t x = 0; x < kW; ++x) {
                const double v = c[std::size_t{y} * kW + x];
                n += v;
                mx += v * x;
                my += v * y;
            }
        }
        mx /= n;
        my /= n;
        double sxx = 0.0;
        double syy = 0.0;
        double sxy = 0.0;
        for (std::uint32_t y = 0; y < kH; ++y) {
            for (std::uint32_t x = 0; x < kW; ++x) {
                const double v = c[std::size_t{y} * kW + x];
                sxx += v * (x - mx) * (x - mx);
                syy += v * (y - my) * (y - my);
                sxy += v * (x - mx) * (y - my);
            }
        }
        return sxy / std::sqrt(sxx * syy);  // the correlation
    };
    std::vector<std::uint8_t> empty;
    double puffs = 0.0;
    double volume = 0.0;
    {
        auto a = make_renderer(10, {}, look);
        if (a == nullptr) {
            GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
        }
        a->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        const std::vector<std::uint8_t> img = draw(*a, view, &f, t, "gaussian-tilted-puffs.png");
        empty = draw(*a, view, nullptr, t, nullptr);
        puffs = lean(cover(img, empty));
    }
    {
        auto b = make_renderer(10, vq, look);
        b->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        volume = lean(cover(draw(*b, view, &f, t, "gaussian-tilted-volume.png"), empty));
        EXPECT_EQ(b->validation_messages(), 0U);
    }
    std::printf("tilted cluster's correlation: puffs %.3f, volume %.3f\n", puffs, volume);
    // Projected on the screen (east across, up down it), its covariance is
    // [[8.5, 7.5], [7.5, 8.5]]: correlation 7.5 / 8.5 = 0.88, negative as y
    // runs down. Measured -0.85 and -0.84 (pixels, perspective); with the
    // whitening's transpose used instead, -0.66.
    EXPECT_NEAR(puffs, -0.882, 0.07);
    EXPECT_NEAR(volume, -0.882, 0.07);
}

// A shadow still darkens the dust in both renderers (D-053, D-054): a
// caster over a receiver, toward the noon sun. They no longer agree on how
// much: the puffs' light is the sun's through Wrenninge's octaves and a sky
// term; the volume's diffuse light holds the sky's and the ground's (D-054),
// which no shadow takes, so its shadow is shallower. Measured on lavapipe:
// shadowed over lit 0.883 (puffs) and 0.977 (volume); without the sun's
// transmittance in the volume's march (a mutation) the volume's is 1.
TEST(ParticlesGaussian, AShadowDarkensTheDustInBothRenderers) {
    const SceneView view = eye_view();
    const LocalTime t = now();
    const auto cluster = [&](ParticleFrame& f, Vec3 at, float sigma, float extinction) {
        Particle& p = f.particles.at(f.count++);
        p.position = at;
        const float s2 = sigma * sigma;
        p.covariance = {s2, 0.0F, 0.0F, s2, 0.0F, s2};
        p.extinction_m2 = extinction;
        p.received = t;
        p.first_seen = t - 10s;
        p.fade_from = t + 10s;
    };
    ParticleFrame alone = frame_at_eye(view, 10);
    cluster(alone, Vec3(0.0F, 40.0F, 3.0F), 1.5F, 20.0F);
    ParticleFrame shadowed = alone;
    // Over it, toward the noon sun: optical depth through its middle
    // extinction / (2 pi sigma^2) = 3.
    cluster(shadowed, Vec3(0.0F, 40.0F, 23.0F), 3.0F, 170.0F);
    ParticleLook look = test_look();
    look.detail = 0.0F;
    ParticleQuality vq;
    vq.volume = true;
    vq.volume_size = 96;
    vq.volume_light_size = 48;
    const auto ratio = [&](Renderer& r) {
        const auto lit = draw(r, view, &alone, t, nullptr);
        const auto dark = draw(r, view, &shadowed, t, nullptr);
        const Rgb x = mean(lit, kW / 2 - 4, kH / 2 - 6, kW / 2 + 4, kH / 2 + 2);
        const Rgb y = mean(dark, kW / 2 - 4, kH / 2 - 6, kW / 2 + 4, kH / 2 + 2);
        return (y.r + y.g + y.b) / (x.r + x.g + x.b);
    };
    double puffs = 0.0;
    double volume = 0.0;
    {
        auto a = make_renderer(10, {}, look);
        if (a == nullptr) {
            GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
        }
        a->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        puffs = ratio(*a);
        EXPECT_EQ(a->validation_messages(), 0U);
    }
    {
        auto b = make_renderer(10, vq, look);
        b->set_sky(sky_bodies(julian_date(2026, 3, 20, 12.0 * 3600.0)));
        volume = ratio(*b);
        EXPECT_EQ(b->validation_messages(), 0U);
    }
    std::printf("shadowed over lit: puffs %.3f, volume %.3f\n", puffs, volume);
    EXPECT_LT(puffs, 0.95);
    EXPECT_LT(volume, 0.99);
    EXPECT_GT(volume, puffs);  // the diffuse light no shadow takes
}

// Multiple scattering as diffusion (D-054). The same dust drawn as a volume
// with the diffusion's sweeps and without (0 sweeps leave the diffuse light
// where it starts, the sky's and the ground's: single scattering alone),
// each settled over several frames, its brightness measured in the middle.
struct ScatterShots {
    Rgb with, without, empty;
    bool vulkan = true;
};
ScatterShots scatter_shots(float extinction, float sigma, std::array<float, 3> albedo,
                           int frames = 24, Vec3 at = Vec3(0.0F, 80.0F, 20.0F),
                           std::array<float, 3> ground = {-1.0F, -1.0F, -1.0F}) {
    const SceneView view = eye_view();
    const LocalTime t = now();
    ParticleFrame f = frame_at_eye(view, 10);
    Particle& p = f.particles.at(f.count++);
    p.position = at;
    const float s2 = sigma * sigma;
    p.covariance = {s2, 0.0F, 0.0F, s2, 0.0F, s2};
    p.extinction_m2 = extinction;
    p.received = t;
    p.first_seen = t - 10s;
    p.fade_from = t + 10s;
    ParticleLook look = test_look();
    look.detail = 0.0F;
    look.albedo = albedo;
    look.ground_albedo = ground;
    ScatterShots out;
    for (const std::uint32_t sweeps : {16U, 0U}) {
        ParticleQuality q;
        q.volume = true;
        q.volume_size = 64;
        q.volume_light_size = 32;
        q.volume_scatter_sweeps = sweeps;
        auto r = make_renderer(10, q, look);
        if (r == nullptr) {
            out.vulkan = false;
            return out;
        }
        // The sun behind the eye (the scene is at 0 N, 0 E, looking north;
        // at noon on 21 December the sun is 66 degrees up in the south):
        // the face seen is the face lit.
        r->set_sky(sky_bodies(julian_date(2026, 12, 21, 12.0 * 3600.0)));
        std::vector<std::uint8_t> img;
        for (int k = 0; k < frames; ++k) {
            img = draw(*r, view, &f, t, nullptr);
        }
        (sweeps > 0 ? out.with : out.without) =
            mean(img, kW / 2 - 4, kH / 2 - 4, kW / 2 + 4, kH / 2 + 4);
        if (sweeps == 0) {
            out.empty = mean(draw(*r, view, nullptr, t, nullptr), kW / 2 - 4, kH / 2 - 4,
                             kW / 2 + 4, kH / 2 + 4);
        }
        EXPECT_EQ(r->validation_messages(), 0U);
    }
    return out;
}
double sum(const Rgb& c) {
    return c.r + c.g + c.b;
}

// A brownout-sized cloud (sigma 15 m, 0.3 per metre at its middle; 4 m
// voxels) around the eye, as brownout is: with the diffusion the dust about
// the eye is brighter, for the light scattered many times in it, when it
// absorbs little each time (dust's albedo); darker when it absorbs half (it
// then holds less than the light arriving from outside). Compounding: single
// scattering would only scale the light by the albedo. And warmer, the blue
// absorbed most each time. Thin dust (0.003 per metre) holds none (D-054's
// weight). The same equations solved on the CPU (the design's check, not
// kept) put the diffuse light 4 to 14 m from its middle at 1.4 to 1.55 times
// the light arriving from outside for dust's albedo, 0.2 to 0.6 for 0.5;
// seen from outside, the light at its edge, where the eye sees in, only up
// to 1.2 and 0.93 (the diffusion matters most inside). Measured on
// lavapipe: printed.
TEST(ParticlesScatter, DustAboutTheEyeIsBrighterForTheLightScatteredInIt) {
    constexpr float kExtinction = 0.3F * 15.749609945722419F * 15.0F * 15.0F * 15.0F;
    const Vec3 around(0.0F, 6.0F, 4.0F);  // the eye 2 m up, 6 m from its middle
    const ScatterShots dust =
        scatter_shots(kExtinction, 15.0F, {0.957F, 0.944F, 0.922F}, 24, around);
    if (!dust.vulkan) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    const ScatterShots dark = scatter_shots(kExtinction, 15.0F, {0.5F, 0.5F, 0.5F}, 24, around);
    const ScatterShots thin =
        scatter_shots(0.01F * kExtinction, 15.0F, {0.957F, 0.944F, 0.922F}, 24, around);
    const double gain = sum(dust.with) - sum(dust.without);
    const double dark_gain = sum(dark.with) - sum(dark.without);
    const double thin_gain = sum(thin.with) - sum(thin.without);
    std::printf(
        "diffusion's gain (sum of levels): dust %.1f, albedo 0.5 %.1f, thin %.2f; "
        "the cloud %.0f with, %.0f without, the scene %.0f; red less blue %.1f with, "
        "%.1f without\n",
        gain, dark_gain, thin_gain, sum(dust.with), sum(dust.without), sum(dust.empty),
        dust.with.r - dust.with.b, dust.without.r - dust.without.b);
    EXPECT_GT(gain, 5.0);
    EXPECT_LT(dark_gain, -5.0);
    EXPECT_LT(std::abs(thin_gain), 1.0);
    EXPECT_GT(dust.with.r - dust.with.b, dust.without.r - dust.without.b);
}

// The ground lights the dust from below (D-054): a desert's tan instead of
// the atmosphere's grey warms the dust about the eye, and brightens it (a
// tan of 0.45, 0.33, 0.22 reflects more than 0.3 in red and green).
TEST(ParticlesScatter, TheGroundColoursTheDust) {
    constexpr float kExtinction = 0.3F * 15.749609945722419F * 15.0F * 15.0F * 15.0F;
    const Vec3 around(0.0F, 6.0F, 4.0F);
    const std::array<float, 3> dust{0.96F, 0.942F, 0.91F};
    const ScatterShots grey = scatter_shots(kExtinction, 15.0F, dust, 24, around);
    if (!grey.vulkan) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    const ScatterShots tan =
        scatter_shots(kExtinction, 15.0F, dust, 24, around, {0.45F, 0.33F, 0.22F});
    std::printf("red less blue about the eye: grey ground %.1f, tan %.1f\n",
                grey.with.r - grey.with.b, tan.with.r - tan.with.b);
    EXPECT_GT(tan.with.r - tan.with.b, grey.with.r - grey.with.b + 10.0);
    EXPECT_GT(tan.with.r, grey.with.r);
}

// The diffuse light settles: sweeps each frame from the last frame's answer
// converge, the frame after the next unchanged; and it starts from the
// light arriving from outside, not from black (its first frame already near).
TEST(ParticlesScatter, TheDiffuseLightSettles) {
    constexpr float kExtinction = 0.3F * 15.749609945722419F * 15.0F * 15.0F * 15.0F;
    const ScatterShots late = scatter_shots(kExtinction, 15.0F, {0.957F, 0.944F, 0.922F}, 40);
    if (!late.vulkan) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    const ScatterShots later = scatter_shots(kExtinction, 15.0F, {0.957F, 0.944F, 0.922F}, 41);
    const ScatterShots first = scatter_shots(kExtinction, 15.0F, {0.957F, 0.944F, 0.922F}, 1);
    std::printf("settling: frame 1 %.1f, 40 %.1f, 41 %.1f\n", sum(first.with), sum(late.with),
                sum(later.with));
    EXPECT_NEAR(sum(later.with), sum(late.with), 1.0);
    EXPECT_GT(sum(first.with), sum(first.without) - 3.0);
}

}  // namespace
}  // namespace fovea
