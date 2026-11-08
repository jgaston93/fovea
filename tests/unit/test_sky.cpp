// The sky (milestone 6 step 2, D-030, D-031): the GPU's tables against a CPU
// integral of the same model, the sky's colours, the sun's disc, and the
// exposure the views share.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/celestial.hpp"
#include "fovea/core/geodesy.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/render/view.hpp"
#include "support/atmosphere_reference.hpp"

namespace fovea {
namespace {

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

// KHSV, 2 m up: the reference scene's eye there.
constexpr Geodetic kKhsv{34.6372, -86.7751, 190.0};

SceneView view_towards(double yaw_deg, double pitch_deg, std::uint32_t w, std::uint32_t h,
                       double half_fov_deg = 30.0, double eye_height_m = 2.0) {
    ReferenceScene scene;
    scene.origin = kKhsv;
    scene.eye_height_m = eye_height_m;
    ViewDesc v;
    v.pose.rotation = {yaw_deg, pitch_deg, 0.0};
    const double half_v = half_fov_deg * static_cast<double>(h) / static_cast<double>(w);
    v.projection.frustum = {-half_fov_deg, half_fov_deg, half_v, -half_v};
    return reference_scene_view(v, scene);
}

// The sun's zenith cosine, elevation and azimuth at the view's eye.
struct SunAt {
    double mu = 0.0;
    double elevation_deg = 0.0;
    double azimuth_deg = 0.0;
};
SunAt sun_at(const SceneView& v, const SkyBodies& sky) {
    const Geodetic g = ecef_to_geodetic(v.eye_ecef);
    const DVec3 ned = glm::transpose(ned_to_ecef(g.lat_deg, g.lon_deg)) * sky.sun;
    SunAt s;
    s.mu = -ned.z;
    s.elevation_deg = glm::degrees(std::asin(-ned.z));
    s.azimuth_deg = glm::degrees(std::atan2(ned.y, ned.x));
    return s;
}

SkyBodies at_utc(unsigned month, unsigned day, double hours) {
    return sky_bodies(julian_date(2026, month, day, hours * 3600.0));
}

double luminance(const std::array<float, 3>& c) {
    return 0.2126 * static_cast<double>(c[0]) + 0.7152 * static_cast<double>(c[1]) +
           0.0722 * static_cast<double>(c[2]);
}

std::array<double, 3> texel(const std::vector<float>& t, std::uint32_t w, std::uint32_t x,
                            std::uint32_t y) {
    const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
    return {static_cast<double>(t[i]), static_cast<double>(t[i + 1]),
            static_cast<double>(t[i + 2])};
}

TEST(Sky, TablesMatchACpuIntegralOfTheSameModel) {
    auto r = make_offscreen({{64, 32}});
    FOVEA_NEED_VULKAN(r);
    const test::AtmosphereReference ref{
        with_visibility(Atmosphere{}, EnvironmentState{}.visibility_m)};
    const SceneView v = view_towards(0.0, 10.0, 64, 32);
    const double eye_r =
        Atmosphere{}.bottom_radius_km + ecef_to_geodetic(v.eye_ecef).alt_m / 1000.0;

    // Every eighth transmittance texel: within 1% where light gets through.
    r->draw_scene_frame(std::vector<SceneView>{v});
    const SkyReadback first = r->read_back_sky(0);
    double worst_t = 0.0;
    for (std::uint32_t y = 0; y < kTransmittanceHeight; y += 4) {
        for (std::uint32_t x = 0; x < kTransmittanceWidth; x += 8) {
            const auto rm = ref.transmittance_r_mu((x + 0.5) / kTransmittanceWidth,
                                                   (y + 0.5) / kTransmittanceHeight);
            const test::Rgb want = ref.transmittance(rm[0], rm[1]);
            const auto got = texel(first.transmittance, kTransmittanceWidth, x, y);
            for (std::size_t c = 0; c < 3; ++c) {
                if (want[c] > 0.01) {
                    worst_t = std::max(worst_t, std::abs(got[c] - want[c]) / want[c]);
                }
            }
        }
    }
    std::printf("transmittance: worst %.3f%%\n", worst_t * 100.0);
    EXPECT_LT(worst_t, 0.01);

    // The sky-view table at a high sun and a setting one: within 3% where
    // the sky is more than a hundredth of its brightest.
    for (const double hours : {17.0, 23.7}) {
        const SkyBodies sky = at_utc(6, 21, hours);
        r->set_sky(sky);
        r->draw_scene_frame(std::vector<SceneView>{v});
        const SkyReadback s = r->read_back_sky(0);
        const double mu_s = sun_at(v, sky).mu;
        double brightest = 0.0;
        for (std::size_t i = 0; i < s.sky_view.size(); i += 4) {
            brightest = std::max(brightest, static_cast<double>(s.sky_view[i + 2]));
        }
        double worst = 0.0;
        for (std::uint32_t y = 0; y < kSkyViewHeight; y += 6) {
            for (std::uint32_t x = 0; x < kSkyViewWidth; x += 12) {
                const auto za = ref.sky_view_angles(eye_r, (x + 0.5) / kSkyViewWidth,
                                                    (y + 0.5) / kSkyViewHeight);
                const test::Rgb want = ref.sky_radiance(eye_r, mu_s, za[0], za[1]);
                const auto got = texel(s.sky_view, kSkyViewWidth, x, y);
                for (std::size_t c = 0; c < 3; ++c) {
                    if (want[c] > 0.01 * brightest) {
                        worst = std::max(worst, std::abs(got[c] - want[c]) / want[c]);
                    }
                }
            }
        }
        std::printf("sky view, sun %.1f degrees up: worst %.2f%%\n", sun_at(v, sky).elevation_deg,
                    worst * 100.0);
        EXPECT_LT(worst, 0.03);
    }
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(Sky, BlueOverheadPalerAtTheHorizonAndRedderTowardASettingSun) {
    auto r = make_offscreen({{64, 32}});
    FOVEA_NEED_VULKAN(r);
    const SceneView v = view_towards(0.0, 10.0, 64, 32);
    // Rows of the sky-view table: 0 is the zenith, kSkyViewHeight / 2 - 1
    // just above the horizon; column 0 looks toward the sun, the last away.
    const std::uint32_t horizon = kSkyViewHeight / 2 - 1;
    const auto blueness = [](const std::array<double, 3>& c) { return c[2] / c[0]; };

    r->set_sky(at_utc(6, 21, 17.0));  // late morning, the sun 70 degrees up
    r->draw_scene_frame(std::vector<SceneView>{v});
    const SkyReadback noon = r->read_back_sky(0);
    // Away from the sun, 60 degrees up (row 10: the rows squeeze toward
    // the horizon), and just above the horizon: the zenith itself is only
    // 15 degrees from this sun, in the haze's bright aureole.
    const auto high = texel(noon.sky_view, kSkyViewWidth, kSkyViewWidth - 1, 10);
    const auto low = texel(noon.sky_view, kSkyViewWidth, kSkyViewWidth - 1, horizon);
    std::printf("noon: 60 degrees up b/r %.2f, horizon b/r %.2f\n", blueness(high), blueness(low));
    EXPECT_GT(blueness(high), 1.5);
    EXPECT_GT(blueness(high), blueness(low) * 1.2);

    r->set_sky(at_utc(6, 22, 0.75));  // 00:45 UTC: 7:45 pm, the sun setting
    r->draw_scene_frame(std::vector<SceneView>{v});
    const SkyReadback dusk = r->read_back_sky(0);
    const auto toward = texel(dusk.sky_view, kSkyViewWidth, 0, horizon);
    const auto overhead = texel(dusk.sky_view, kSkyViewWidth, kSkyViewWidth / 2, 0);
    std::printf("sunset: toward the sun r/b %.2f, overhead b/r %.2f\n", toward[0] / toward[2],
                blueness(overhead));
    EXPECT_GT(toward[0], toward[2]);     // red toward the sun
    EXPECT_GT(blueness(overhead), 1.0);  // still blue overhead
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(Sky, TheSunsDiscIsWhereTheSunIs) {
    constexpr std::uint32_t kSize = 256;
    auto r = make_offscreen({{kSize, kSize}});
    FOVEA_NEED_VULKAN(r);
    const SkyBodies sky = at_utc(3, 20, 15.5);
    // Toward the sun, a few degrees off, so it lands off the centre.
    const SceneView probe = view_towards(0.0, 0.0, kSize, kSize);
    const SunAt sun = sun_at(probe, sky);
    const SceneView v =
        view_towards(sun.azimuth_deg + 4.0, sun.elevation_deg - 3.0, kSize, kSize, 10.0);
    r->set_sky(sky);
    r->draw_scene_frame(std::vector<SceneView>{v});
    const auto px = r->read_back_rgba(0);
    // Where the view's matrix puts the sun, and the centre of the disc's
    // saturated pixels.
    const glm::dvec4 clip = glm::dmat4(v.ecef_to_clip) * glm::dvec4(sky.sun * 1e6, 1.0);
    const double want_x = (clip.x / clip.w + 1.0) * 0.5 * kSize;
    const double want_y = (clip.y / clip.w + 1.0) * 0.5 * kSize;
    double sx = 0.0;
    double sy = 0.0;
    int n = 0;
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
            if (px[i] >= 250 && px[i + 1] >= 250 && px[i + 2] >= 240) {
                sx += x + 0.5;
                sy += y + 0.5;
                ++n;
            }
        }
    }
    ASSERT_GT(n, 0) << "no disc";
    std::printf("sun disc: %d pixels, centre (%.2f, %.2f), expected (%.2f, %.2f)\n", n, sx / n,
                sy / n, want_x, want_y);
    EXPECT_NEAR(sx / n, want_x, 1.0);
    EXPECT_NEAR(sy / n, want_y, 1.0);
    EXPECT_LT(n, 200);  // a disc, not a glow
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(Sky, EveryViewTakesTheSameExposure) {
    auto r = make_offscreen({{64, 32}, {64, 32}, {32, 64}});
    FOVEA_NEED_VULKAN(r);
    r->set_sky(at_utc(3, 20, 15.5));
    const std::vector<SceneView> views = {view_towards(-60.0, 0.0, 64, 32),
                                          view_towards(0.0, 20.0, 64, 32),
                                          view_towards(90.0, -30.0, 32, 64)};
    for (int f = 0; f < 3; ++f) {
        r->draw_scene_frame(views);
    }
    const float e0 = r->read_back_sky(0).exposure;
    EXPECT_GT(e0, 0.0F);
    EXPECT_EQ(r->read_back_sky(1).exposure, e0);
    EXPECT_EQ(r->read_back_sky(2).exposure, e0);
    EXPECT_EQ(r->validation_messages(), 0U);
}

// The light follows the sun (milestone 6 step 1, now through the air): a
// level surface gets more at noon than at 8 am, and none from the sun at
// night; each first frame takes its exposure at once, so it is exactly the
// key over the light (the sun's facing it, and the sky's), or over night's
// floor.
TEST(Sky, TheLightFollowsTheDateAndTime) {
    const SceneView v = view_towards(0.0, 10.0, 64, 32);
    struct Light {
        double level = 0.0;
        double light = 0.0;  // facing the sun, and the sky's: what exposure follows
        double exposure = 0.0;
    };
    const auto light_at = [&](double hours) -> std::optional<Light> {
        auto r = make_offscreen({{64, 32}});
        if (r == nullptr) {
            return std::nullopt;
        }
        const SkyBodies sky = at_utc(6, 21, hours);
        r->set_sky(sky);
        r->draw_scene_frame(std::vector<SceneView>{v});
        const SkyReadback s = r->read_back_sky(0);
        EXPECT_EQ(r->validation_messages(), 0U);
        Light l;
        l.light = luminance(s.sun_irradiance) + luminance(s.moon_irradiance) +
                  luminance(s.sky_irradiance);
        l.level = luminance(s.sun_irradiance) * std::max(sun_at(v, sky).mu, 0.0) +
                  luminance(s.sky_irradiance);
        l.exposure = static_cast<double>(s.exposure);
        return l;
    };
    const auto noon = light_at(18.0);
    if (!noon) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    const auto morning = light_at(13.0);
    const auto night = light_at(6.0);
    std::printf("level light: noon %.1f, 8 am %.1f, night %.4f\n", noon->level, morning->level,
                night->level);
    EXPECT_GT(noon->level, morning->level * 1.5);
    EXPECT_LT(night->level, noon->level * 1e-4);
    // The exposure's key (pi) over that light. Below the knee, a twentieth
    // of the sun's illuminance, it adapts three quarters of a stop a stop
    // (step 4): key / (knee (light / knee)^0.75).
    const double key = 3.14159265358979;
    EXPECT_NEAR(noon->exposure * noon->light, key, key * 1e-3);
    const double knee = 0.05 * Atmosphere{}.sun_illuminance[0];
    for (const auto& l : {*morning, *night}) {
        const double adapted = l.light >= knee ? l.light : knee * std::pow(l.light / knee, 0.75);
        EXPECT_NEAR(l.exposure, key / adapted, key / adapted * 1e-3) << l.light;
    }
    std::printf("night: light %.3g, %.1f stops darker than full adaptation\n", night->light,
                std::log2(key / night->light / night->exposure));
}

// Visibility sets the haze (milestone 6 step 3, D-030): along a level ray
// from sea level, the air's transmittance falls to about 2% at the set
// visibility (Koschmieder: a black object's contrast against the horizon
// sky is the air's transmittance), and doubling the visibility moves that
// distance with it. Read from the aerial-perspective volume (8-bit images
// can't show a 2% contrast); within a factor of 1.5, the volume holding the
// mean of red, green and blue where Koschmieder's is green's.
TEST(Sky, VisibilityIsWhereTheHazeLeavesTwoPercent) {
    auto r = make_offscreen({{64, 64}});
    FOVEA_NEED_VULKAN(r);
    ReferenceScene scene;
    scene.origin = {34.6372, -86.7751, 0.0};  // at sea level, where visibility is set
    scene.eye_height_m = 2.0;
    ViewDesc d;
    // A narrow view, so the froxel rows near its middle are level.
    d.projection.frustum = {-2.0, 2.0, 2.0, -2.0};
    const SceneView v = reference_scene_view(d, scene);
    r->set_sky(at_utc(6, 21, 17.0));
    for (const double visibility_km : {20.0, 40.0}) {
        r->set_visibility(visibility_km * 1000.0);
        r->draw_scene_frame(std::vector<SceneView>{v});
        const SkyReadback s = r->read_back_sky(0);
        // The two middle columns' transmittance in the two rows just above
        // level (those below meet the ground at the horizon, 5 km from a
        // 2 m eye) at each slice's far edge, then where it crosses 2%.
        std::vector<double> t(kAerialDepth);
        for (std::uint32_t k = 0; k < kAerialDepth; ++k) {
            double sum = 0.0;
            for (std::uint32_t y = 14; y <= 15; ++y) {
                for (std::uint32_t x = 15; x <= 16; ++x) {
                    const std::size_t i =
                        ((static_cast<std::size_t>(k) * kAerialHeight + y) * kAerialWidth + x) * 4;
                    sum += static_cast<double>(s.aerial[i + 3]);
                }
            }
            t[k] = sum / 4.0;
        }
        const auto far_edge = [](double k) {
            const double w = (k + 1.0) / kAerialDepth;
            return w * w * kAerialDistanceKm;
        };
        double at = -1.0;
        for (std::uint32_t k = 1; k < kAerialDepth; ++k) {
            if (t[k] <= 0.02 && t[k - 1] > 0.02) {
                // Between the two edges, by the log of the transmittance.
                const double f = std::log(t[k - 1] / 0.02) / std::log(t[k - 1] / t[k]);
                at = far_edge(k - 1) + f * (far_edge(k) - far_edge(k - 1));
                break;
            }
        }
        std::printf("visibility %.0f km: transmittance 2%% at %.1f km\n", visibility_km, at);
        EXPECT_GT(at, visibility_km / 1.5);
        EXPECT_LT(at, visibility_km * 1.5);
    }
    EXPECT_EQ(r->validation_messages(), 0U);
}

// The haze reaches what is drawn: seen from 300 m up, the reference scene's
// ground a few rows below the horizon (about 5 km away) all but melts into
// the sky above the horizon at 500 m visibility, and stands apart from it
// at 200 km.
TEST(Sky, FarGroundFadesIntoTheSkyAsVisibilityFalls) {
    constexpr std::uint32_t kW = 96;
    constexpr std::uint32_t kH = 96;
    const SceneView v = view_towards(0.0, 0.0, kW, kH, 30.0, 300.0);
    // A level view: the horizon is just below the middle row (dipping
    // 0.6 degrees from 300 m); 6 rows either side of the middle. A renderer
    // for each visibility, so each takes its exposure at once.
    const auto difference = [&](double visibility_m) -> double {
        auto r = make_offscreen({{kW, kH}});
        if (r == nullptr) {
            return -1.0;
        }
        r->set_sky(at_utc(6, 21, 17.0));
        r->set_visibility(visibility_m);
        r->draw_scene_frame(std::vector<SceneView>{v});
        const auto px = r->read_back_rgba(0);
        EXPECT_EQ(r->validation_messages(), 0U);
        const auto at = [&](std::uint32_t y, std::uint32_t c) {
            return static_cast<double>(px[(static_cast<std::size_t>(y) * kW + kW / 2) * 4 + c]);
        };
        double d = 0.0;
        for (std::uint32_t c = 0; c < 3; ++c) {
            d += std::abs(at(kH / 2 + 6, c) - at(kH / 2 - 6, c));
        }
        return d;
    };
    const double thick = difference(500.0);
    if (thick < 0.0) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    const double clear = difference(200000.0);
    std::printf("ground against sky: %.0f at 500 m visibility, %.0f at 200 km\n", thick, clear);
    EXPECT_LT(thick, 30.0);
    EXPECT_GT(clear, 3.0 * thick);
}

// The sun's elevation at the view's eye, degrees.
double sun_elevation(const SceneView& v, const SkyBodies& sky) {
    return sun_at(v, sky).elevation_deg;
}

// Night falls (milestone 6 step 4): from noon through sunset and civil
// twilight to midnight the sky's light on level ground falls, in that
// order; stars show at midnight and not by day or at sunset. KHSV, the
// June solstice, the moon down at midnight.
TEST(Sky, NightFallsFromNoonThroughTwilightAndTheStarsComeOut) {
    constexpr std::uint32_t kSize = 128;
    const SceneView v = view_towards(0.0, 50.0, kSize, kSize);
    // Sunset and the end of civil twilight (6 degrees down), found by the
    // minute after 00:00 UTC on the 22nd.
    const auto first_below = [&](double elevation) {
        for (int minute = 0; minute < 240; ++minute) {
            const double hours = 24.0 + minute / 60.0;
            if (sun_elevation(v, at_utc(6, 21, hours)) < elevation) {
                return hours;
            }
        }
        return 24.0;
    };
    struct Look {
        double sky = 0.0;
        int stars = 0;
    };
    const auto look_at = [&](double hours) -> std::optional<Look> {
        auto r = make_offscreen({{kSize, kSize}});
        if (r == nullptr) {
            return std::nullopt;
        }
        r->set_sky(at_utc(6, 21, hours));
        r->draw_scene_frame(std::vector<SceneView>{v});
        const SkyReadback s = r->read_back_sky(0);
        const auto px = r->read_back_rgba(0);
        EXPECT_EQ(r->validation_messages(), 0U);
        // A star: a pixel well brighter than the mean of its eight neighbours.
        const auto at = [&](std::uint32_t x, std::uint32_t y) {
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
            return static_cast<double>(px[i]) + px[i + 1] + px[i + 2];
        };
        Look l;
        l.sky = luminance(s.sky_irradiance);
        for (std::uint32_t y = 1; y + 1 < kSize; ++y) {
            for (std::uint32_t x = 1; x + 1 < kSize; ++x) {
                double around = 0.0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        around += (dx == 0 && dy == 0)
                                      ? 0.0
                                      : at(static_cast<std::uint32_t>(static_cast<int>(x) + dx),
                                           static_cast<std::uint32_t>(static_cast<int>(y) + dy));
                    }
                }
                l.stars += at(x, y) > around / 8.0 + 60.0 ? 1 : 0;
            }
        }
        return l;
    };
    const auto noon = look_at(18.0);
    if (!noon) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    const auto sunset = look_at(first_below(0.0));
    const auto civil = look_at(first_below(-6.0));
    const auto midnight = look_at(6.0);
    std::printf("sky light: noon %.4g, sunset %.4g, civil twilight %.4g, midnight %.4g\n",
                noon->sky, sunset->sky, civil->sky, midnight->sky);
    std::printf("stars: noon %d, sunset %d, civil twilight %d, midnight %d\n", noon->stars,
                sunset->stars, civil->stars, midnight->stars);
    EXPECT_GT(noon->sky, sunset->sky);
    EXPECT_GT(sunset->sky, civil->sky);
    EXPECT_GT(civil->sky, midnight->sky);
    EXPECT_EQ(noon->stars, 0);
    EXPECT_EQ(sunset->stars, 0);
    EXPECT_GT(midnight->stars, 20);
}

// The moon's lit side faces the sun (step 4): a waxing half moon in the
// south-west at 03:00 UTC on the 22nd (the sun 18 degrees down, in the
// north-west), seen close up: the brightness of its disc leans toward
// where the sun is, across the sky.
TEST(Sky, TheMoonsLitSideFacesTheSun) {
    constexpr std::uint32_t kSize = 128;
    const SkyBodies sky = at_utc(6, 22, 3.0);
    const SceneView probe = view_towards(0.0, 0.0, kSize, kSize);
    const Geodetic g = ecef_to_geodetic(probe.eye_ecef);
    const DMat3 to_ned = glm::transpose(ned_to_ecef(g.lat_deg, g.lon_deg));
    const DVec3 moon = glm::normalize(sky.moon_m - probe.eye_ecef);
    const DVec3 m = to_ned * moon;
    const double el = glm::degrees(std::asin(-m.z));
    const double az = glm::degrees(std::atan2(m.y, m.x));
    ASSERT_GT(el, 5.0) << "the moon is down";
    // A 1-degree view, the moon (half a degree) in its middle.
    const SceneView v = view_towards(az, el, kSize, kSize, 0.5);
    auto r = make_offscreen({{kSize, kSize}});
    FOVEA_NEED_VULKAN(r);
    r->set_sky(sky);
    r->draw_scene_frame(std::vector<SceneView>{v});
    const auto px = r->read_back_rgba(0);
    const auto project = [&](const DVec3& dir) {
        const glm::dvec4 c = glm::dmat4(v.ecef_to_clip) * glm::dvec4(dir * 1e6, 1.0);
        return glm::dvec2((c.x / c.w + 1.0) * 0.5 * kSize, (c.y / c.w + 1.0) * 0.5 * kSize);
    };
    // Toward the sun across the sky from the moon, on the screen.
    const DVec3 toward = glm::normalize(sky.sun - moon * glm::dot(sky.sun, moon));
    const glm::dvec2 centre = project(moon);
    const glm::dvec2 sunward = glm::normalize(project(moon + toward * 1e-3) - centre);
    // The disc's brightness-weighted centre, against its geometric one.
    double sum = 0.0;
    glm::dvec2 weighted(0.0);
    for (std::uint32_t y = 0; y < kSize; ++y) {
        for (std::uint32_t x = 0; x < kSize; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * kSize + x) * 4;
            const double b = static_cast<double>(px[i]) + px[i + 1] + px[i + 2];
            if (b > 300.0) {
                sum += b;
                weighted += b * glm::dvec2(x + 0.5, y + 0.5);
            }
        }
    }
    ASSERT_GT(sum, 0.0) << "no moon";
    const glm::dvec2 lean = weighted / sum - centre;
    std::printf("moon %.1f degrees up, %.0f%% lit; its light leans %.1f px, %.2f toward the sun\n",
                el, sky.moon_lit * 100.0, glm::length(lean),
                glm::dot(glm::normalize(lean), sunward));
    EXPECT_GT(glm::dot(glm::normalize(lean), sunward), 0.8);
    EXPECT_GT(glm::length(lean), 3.0);
    EXPECT_EQ(r->validation_messages(), 0U);
}

}  // namespace
}  // namespace fovea
