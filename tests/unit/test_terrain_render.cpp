// Terrain drawn by the renderer (milestone 3 step 5), on databases the
// tests write: imagery whose colours change at known latitudes and
// longitudes must show those edges where the view's own projection, in
// double precision on the CPU, puts them; a golden image of hilly terrain
// catches any other change.

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/geodesy.hpp"
#include "fovea/core/image_file.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/terrain/database.hpp"
#include "fovea/terrain/tile_file.hpp"
#include "fovea/terrain/tile_key.hpp"

namespace fovea {
namespace {

namespace fs = std::filesystem;

const fs::path& test_root() {
    struct Root {
        fs::path path;
        Root()
            : path(fs::temp_directory_path() /
                   ("fovea-terrain-render-" + std::to_string(std::random_device{}()))) {}
        ~Root() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };
    static const Root root;
    return root.path;
}

using Rgb = std::array<std::uint8_t, 3>;

// A database of one root tile at `root` and its children down to `finest`:
// heights height(lat, lon) at every sample, imagery colour(lat, lon) at
// every texel centre (RGBA8, mips averaged four texels at a time). Every
// tile but the finest gets error `root_error` halving with each level, so
// near tiles split.
struct Spec {
    std::uint8_t root = 15;
    std::uint8_t finest = 16;
    std::uint32_t samples = 17;
    std::uint32_t texels = 256;
    double root_error = 50.0;
    TileKey root_key;
    std::function<double(double, double)> height;
    std::function<Rgb(double, double)> colour;
};

fs::path write_db(const std::string& name, const Spec& s) {
    const fs::path dir = test_root() / name;
    DatabaseManifest m;
    m.name = name;
    m.area = tile_bounds(s.root_key);
    m.elevation_samples = s.samples;
    m.imagery = DatabaseManifest::Imagery{ImageryFormat::rgba8_srgb, s.texels};
    for (std::uint8_t lv = s.root; lv <= s.finest; ++lv) {
        const std::uint32_t f = 1U << (lv - s.root);
        const LevelTiles t{lv, s.root_key.x * f, s.root_key.x * f + f - 1, s.root_key.y * f,
                           s.root_key.y * f + f - 1};
        m.levels.push_back(t);
        for (std::size_t i = 0; i < t.count(); ++i) {
            const TileKey key = t.at(i);
            const GeoBounds b = tile_bounds(key);
            ElevationGrid g;
            g.size = s.samples;
            g.error_m =
                lv == s.finest ? 0.0F : static_cast<float>(s.root_error / std::exp2(lv - s.root));
            const double step = (b.east_deg - b.west_deg) / (s.samples - 1);
            for (std::uint32_t r = 0; r < s.samples; ++r) {
                for (std::uint32_t c = 0; c < s.samples; ++c) {
                    g.heights.push_back(static_cast<float>(
                        s.height(b.south_deg + r * step, b.west_deg + c * step)));
                }
            }
            const auto [lo, hi] = std::minmax_element(g.heights.begin(), g.heights.end());
            g.min_m = *lo;
            g.max_m = *hi;
            ImageryImage im;
            im.format = ImageryFormat::rgba8_srgb;
            im.size = s.texels;
            std::vector<std::uint8_t> mip(std::size_t{s.texels} * s.texels * 4);
            const double texel = (b.east_deg - b.west_deg) / s.texels;
            for (std::uint32_t r = 0; r < s.texels; ++r) {
                for (std::uint32_t c = 0; c < s.texels; ++c) {
                    const Rgb rgb =
                        s.colour(b.south_deg + (r + 0.5) * texel, b.west_deg + (c + 0.5) * texel);
                    const std::size_t at = (std::size_t{r} * s.texels + c) * 4;
                    mip[at] = rgb[0];
                    mip[at + 1] = rgb[1];
                    mip[at + 2] = rgb[2];
                    mip[at + 3] = 255;
                }
            }
            for (std::uint32_t size = s.texels;; size /= 2) {
                im.mips.push_back(mip);
                if (size == 1) {
                    break;
                }
                const std::uint32_t h = size / 2;
                std::vector<std::uint8_t> next(std::size_t{h} * h * 4);
                for (std::uint32_t r = 0; r < h; ++r) {
                    for (std::uint32_t c = 0; c < h; ++c) {
                        for (std::size_t ch = 0; ch < 4; ++ch) {
                            const auto v = [&](std::uint32_t rr, std::uint32_t cc) {
                                return int{mip[(std::size_t{rr} * size + cc) * 4 + ch]};
                            };
                            next[(std::size_t{r} * h + c) * 4 + ch] = static_cast<std::uint8_t>(
                                (v(2 * r, 2 * c) + v(2 * r, 2 * c + 1) + v(2 * r + 1, 2 * c) +
                                 v(2 * r + 1, 2 * c + 1) + 2) /
                                4);
                        }
                    }
                }
                mip = std::move(next);
            }
            EXPECT_TRUE(
                write_tile(dir / tile_path(key), {key, {encode_elevation(g), encode_imagery(im)}}));
        }
    }
    EXPECT_TRUE(write_manifest(dir, m));
    return dir;
}

std::unique_ptr<Renderer> make_renderer(std::uint32_t w, std::uint32_t h) {
    RendererOptions options;
    if (std::getenv("FOVEA_REQUIRE_VALIDATION") != nullptr) {
        options.validation = Validation::required;
    }
    try {
        return Renderer::offscreen(std::vector<OffscreenOutput>{{w, h}}, options);
    } catch (const std::runtime_error& e) {
        if (std::getenv("FOVEA_REQUIRE_VULKAN") != nullptr) {
            ADD_FAILURE() << "no Vulkan: " << e.what();
        }
        return nullptr;
    }
}

#define RENDERER_OR_SKIP(r, w, h)                               \
    auto r = make_renderer(w, h);                               \
    if (!(r)) {                                                 \
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine"; \
    }

std::unique_ptr<TerrainDatabase> open_db(const fs::path& dir) {
    std::unique_ptr<TerrainDatabase> db;
    const auto r = TerrainDatabase::open(dir, db);
    EXPECT_TRUE(r.errors.empty()) << (r.errors.empty() ? "" : r.errors[0]);
    return db;
}

// The view: from `eye_height_m` above `origin` (ellipsoid heights here: no
// host, no geoid), turned by yaw and pitch, a 60 x 60 degree frustum.
SceneView view_at(const Geodetic& origin, double eye_height_m, double pitch_deg,
                  double yaw_deg = 0.0) {
    ReferenceScene scene;
    scene.origin = origin;
    scene.eye_height_m = eye_height_m;
    ViewDesc v;
    v.pose.rotation = {yaw_deg, pitch_deg, 0.0};
    v.projection.frustum = {-30.0, 30.0, 30.0, -30.0};
    return reference_scene_view(v, scene);
}

// Where a point shows in a w x h image (row 0 at the top), by the view's
// matrix in double.
glm::dvec2 project(const SceneView& v, const Geodetic& g, std::uint32_t w, std::uint32_t h) {
    const DVec3 rel = geodetic_to_ecef(g) - v.eye_ecef;
    const glm::dvec4 clip = glm::dmat4(v.ecef_to_clip) * glm::dvec4(rel, 1.0);
    return {(clip.x / clip.w + 1.0) * 0.5 * w, (clip.y / clip.w + 1.0) * 0.5 * h};
}

// The strongest channel of a pixel (0 red, 1 green, 2 blue), or 3 for
// white.
int kind_at(const std::vector<std::uint8_t>& px, std::uint32_t w, int x, int y) {
    const std::size_t at = (static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 4;
    const std::array<int, 3> c = {px[at], px[at + 1], px[at + 2]};
    if (std::min({c[0], c[1], c[2]}) > 150) {
        return 3;
    }
    return static_cast<int>(std::max_element(c.begin(), c.end()) - c.begin());
}

// Quadrants meeting at (lat0, lon0): red south-west, green north-west, blue
// south-east, white north-east.
Rgb quadrant(double lat, double lon, double lat0, double lon0) {
    if (lon < lon0) {
        return lat < lat0 ? Rgb{220, 30, 30} : Rgb{30, 220, 30};
    }
    return lat < lat0 ? Rgb{30, 30, 220} : Rgb{235, 235, 235};
}

// Checks that the four colours meet where the view puts (lat0, lon0, h0):
// each quadrant's colour a few pixels from the expected point, on its side.
void expect_corner_at(const std::vector<std::uint8_t>& px, std::uint32_t w, glm::dvec2 at,
                      bool north_up) {
    const int x = static_cast<int>(std::lround(at.x));
    const int y = static_cast<int>(std::lround(at.y));
    constexpr int d = 3;  // pixels either way: a texel is about one
    // Screen up is north here (the view looks north); left is west.
    const int north = north_up ? -d : d;
    EXPECT_EQ(kind_at(px, w, x - d, y - north), 0) << "south-west of " << x << ", " << y;
    EXPECT_EQ(kind_at(px, w, x - d, y + north), 1) << "north-west of " << x << ", " << y;
    EXPECT_EQ(kind_at(px, w, x + d, y - north), 2) << "south-east of " << x << ", " << y;
    EXPECT_EQ(kind_at(px, w, x + d, y + north), 3) << "north-east of " << x << ", " << y;
    // And a pixel further than that in each direction still agrees, so the
    // corner is within d of the expected point, not just near it.
    EXPECT_EQ(kind_at(px, w, x - 2 * d, y), kind_at(px, w, x - d, y));
}

Spec quadrant_spec(const std::function<double(double, double)>& height, double* lat0,
                   double* lon0) {
    Spec s;
    s.root_key = tile_at(34.637, -86.775, s.root);
    const GeoBounds b = tile_bounds(s.root_key);
    // Inside a level-16 tile, not on any tile edge.
    *lat0 = b.south_deg + 0.37 * (b.north_deg - b.south_deg);
    *lon0 = b.west_deg + 0.41 * (b.east_deg - b.west_deg);
    const double la = *lat0;
    const double lo = *lon0;
    s.height = height;
    s.colour = [la, lo](double lat, double lon) { return quadrant(lat, lon, la, lo); };
    return s;
}

TEST(TerrainRender, FlatGroundShowsWhereTheProjectionPutsIt) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    const Spec s = quadrant_spec([](double, double) { return 120.0; }, &lat0, &lon0);
    const auto db = open_db(write_db("flat", s));
    constexpr std::uint32_t kSize = 256;
    RENDERER_OR_SKIP(r, kSize, kSize);
    const TerrainLoad load = r->load_terrain(*db);
    EXPECT_EQ(load.tiles, 5U);
    EXPECT_TRUE(load.problems.empty()) << load.problems[0];
    // Looking straight down from 300 m, 40 m south-west of the corner.
    const DMat3 ned = ned_to_ecef(lat0, lon0);
    const Geodetic eye_at =
        ecef_to_geodetic(geodetic_to_ecef({lat0, lon0, 120.0}) + ned * DVec3(-40.0, -40.0, 0.0));
    const SceneView v = view_at({eye_at.lat_deg, eye_at.lon_deg, 120.0}, 300.0, -90.0);
    r->draw_scene_frame(std::vector<SceneView>{v});
    EXPECT_GT(r->terrain_tiles_drawn(), 0U);
    EXPECT_EQ(r->validation_messages(), 0U);
    const auto px = r->read_back_rgba(0);
    expect_corner_at(px, kSize, project(v, {lat0, lon0, 120.0}, kSize, kSize), true);
}

TEST(TerrainRender, HeightsLiftTheGroundWhereTheyShould) {
    // Ground rising 1 m in 10 northwards, seen at 45 degrees: had the
    // heights been taken as flat, the corner would show tens of pixels off.
    double lat0 = 0.0;
    double lon0 = 0.0;
    const GeoBounds rb = tile_bounds(tile_at(34.637, -86.775, 15));
    const double la = rb.south_deg + 0.37 * (rb.north_deg - rb.south_deg);  // as quadrant_spec
    const auto rise = [la](double lat, double) {
        return 150.0 + 0.1 * (lat - la) * 111000.0;  // about 1 in 10
    };
    const Spec s = quadrant_spec(rise, &lat0, &lon0);
    const auto db = open_db(write_db("slope", s));
    constexpr std::uint32_t kSize = 256;
    RENDERER_OR_SKIP(r, kSize, kSize);
    (void)r->load_terrain(*db);
    const double h0 = rise(lat0, lon0);
    const DMat3 ned = ned_to_ecef(lat0, lon0);
    const Geodetic eye_at =
        ecef_to_geodetic(geodetic_to_ecef({lat0, lon0, h0}) + ned * DVec3(-200.0, 0.0, 0.0));
    const SceneView v = view_at({eye_at.lat_deg, eye_at.lon_deg, h0}, 200.0, -45.0);
    r->draw_scene_frame(std::vector<SceneView>{v});
    EXPECT_EQ(r->validation_messages(), 0U);
    const auto px = r->read_back_rgba(0);
    const glm::dvec2 at = project(v, {lat0, lon0, h0}, kSize, kSize);
    EXPECT_NEAR(at.x, kSize / 2.0, 2.0);  // in the middle: the eye looks at it
    EXPECT_NEAR(at.y, kSize / 2.0, 2.0);
    expect_corner_at(px, kSize, at, true);
    // Without its height (on the ellipsoid) it would show far off.
    EXPECT_GT(std::abs(project(v, {lat0, lon0, 0.0}, kSize, kSize).y - at.y), 20.0);
}

TEST(TerrainRender, LoadingSkipsDamagedTilesAndStopsAtMaxLevel) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    const Spec s = quadrant_spec([](double, double) { return 120.0; }, &lat0, &lon0);
    const fs::path dir = write_db("damaged", s);
    const TileKey broken = children(s.root_key)[2];
    std::ofstream(dir / tile_path(broken), std::ios::binary | std::ios::trunc) << "not a tile";
    const auto db = open_db(dir);
    RENDERER_OR_SKIP(r, 64, 64);
    const TerrainLoad load = r->load_terrain(*db);
    EXPECT_EQ(load.tiles, 4U);
    ASSERT_EQ(load.problems.size(), 1U);
    EXPECT_NE(load.problems[0].find(to_string(broken)), std::string::npos) << load.problems[0];
    // The root can't split (a child is missing), and is drawn whole.
    r->draw_scene_frame(std::vector<SceneView>{view_at({lat0, lon0, 120.0}, 100.0, -90.0)});
    EXPECT_EQ(r->terrain_tiles_drawn(), 1U);
    EXPECT_EQ(r->validation_messages(), 0U);
    EXPECT_THROW((void)r->load_terrain(*db), std::runtime_error);  // once only

    r.reset();  // one Renderer at a time
    RENDERER_OR_SKIP(r2, 64, 64);
    EXPECT_EQ(r2->load_terrain(*db, {15, 1.0}).tiles, 1U);
}

// Hills and a pattern from 400 m: everything together, as a golden image
// (FOVEA_UPDATE_GOLDEN=1 rewrites it, on lavapipe, as test_golden.cpp).
TEST(TerrainRender, HillsMatchTheGoldenImage) {
    Spec s;
    s.finest = 17;
    s.samples = 33;
    s.texels = 128;
    s.root_key = tile_at(34.637, -86.775, s.root);
    const GeoBounds b = tile_bounds(s.root_key);
    const double clat = (b.south_deg + b.north_deg) / 2;
    const double clon = (b.west_deg + b.east_deg) / 2;
    s.height = [clat, clon](double lat, double lon) {
        const double n = (lat - clat) * 111000.0;
        const double e = (lon - clon) * 91000.0;
        return 150.0 + 30.0 * std::sin(n / 45.0) * std::cos(e / 35.0) + 0.05 * n;
    };
    // Smooth (waves dozens of texels long): GPUs filter textures
    // differently (anisotropic or not), which hard edges would turn into
    // large differences; texel placement is pinned by the tests above.
    s.colour = [](double lat, double lon) {
        const auto wave = [](double v) { return static_cast<std::uint8_t>(127.5 + 100.0 * v); };
        return Rgb{wave(std::sin(lat * 9000.0)), wave(std::cos(lon * 7000.0)),
                   wave(std::sin((lat + lon) * 5000.0))};
    };
    const auto db = open_db(write_db("hills", s));
    constexpr std::uint32_t kW = 320;
    constexpr std::uint32_t kH = 200;
    RENDERER_OR_SKIP(r, kW, kH);
    (void)r->load_terrain(*db);
    const SceneView v = view_at({b.south_deg + 0.0004, clon, 150.0}, 250.0, -30.0, 10.0);
    r->draw_scene_frame(std::vector<SceneView>{v});
    EXPECT_EQ(r->validation_messages(), 0U);
    const RgbaImage image{kW, kH, r->read_back_rgba(0)};
    const fs::path file = FOVEA_SOURCE_DIR "/tests/data/golden/terrain_hills.png";
    if (std::getenv("FOVEA_UPDATE_GOLDEN") != nullptr) {
        ASSERT_NE(r->gpu().name.find("llvmpipe"), std::string::npos)
            << "FOVEA_UPDATE_GOLDEN on " << r->gpu().name << ": set VK_DRIVER_FILES to lavapipe";
        ASSERT_TRUE(write_png(file, image));
        return;
    }
    const auto golden = read_png(file);
    ASSERT_TRUE(golden.has_value()) << "missing " << file << " (FOVEA_UPDATE_GOLDEN=1 makes it)";
    const ImageDifference d = compare_images(image, *golden, 4);
    ASSERT_TRUE(d.same_size);
    if (d.fraction_over > 0.01) {
        // What this GPU drew, to look at beside the golden.
        const fs::path actual = fs::temp_directory_path() / "terrain_hills.actual.png";
        EXPECT_TRUE(write_png(actual, image));
        std::printf("wrote %s\n", actual.string().c_str());
    }
    // As the reference scene's goldens (test_golden.cpp), with room for
    // GPUs' texture filtering, which differs more than flat shading.
    EXPECT_LE(d.fraction_over, 0.01) << d.fraction_over * 100.0 << "% of pixels differ (mean "
                                     << d.mean_abs << ", max " << int{d.max_abs} << ")";
}

}  // namespace
}  // namespace fovea
