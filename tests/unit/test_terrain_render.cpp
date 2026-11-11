// Terrain drawn by the renderer (milestone 3 step 5), on databases the
// tests write: imagery whose colours change at known latitudes and
// longitudes must show those edges where the view's own projection, in
// double precision on the CPU, puts them; a golden image of hilly terrain
// catches any other change.

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/geodesy.hpp"
#include "fovea/core/image_file.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/render/shadow_cascades.hpp"
#include "fovea/terrain/database.hpp"
#include "fovea/terrain/tile_file.hpp"
#include "fovea/terrain/tile_key.hpp"
#include "support/alloc_counter.hpp"
#include "support/glb.hpp"

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
    // Placed models (milestone 4 step 3): in the root tile, their files in
    // models/.
    std::vector<Placement> placements;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> models;
    // Airport lights (milestone 6 step 8): in the root tile too.
    std::vector<TileLight> lights;
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
            TileFile tile{key, {encode_elevation(g), encode_imagery(im)}};
            if (lv == s.root && !s.placements.empty()) {
                tile.layers.push_back(*encode_placements(s.placements));
            }
            if (lv == s.root && !s.lights.empty()) {
                tile.layers.push_back(*encode_lights(s.lights));
            }
            EXPECT_TRUE(write_tile(dir / tile_path(key), tile));
        }
    }
    if (!s.placements.empty()) {
        m.placements = DatabaseManifest::Placements{s.root};
    }
    if (!s.lights.empty()) {
        m.lights = DatabaseManifest::Lights{s.root};
    }
    for (const auto& [model, bytes] : s.models) {
        EXPECT_TRUE(write_bytes_atomically(dir / model_path(model), bytes));
    }
    EXPECT_TRUE(write_manifest(dir, m));
    return dir;
}

struct RenderOptionsNoValidation {
    RendererOptions options{Validation::off};
};

std::unique_ptr<Renderer> make_renderer(std::uint32_t w, std::uint32_t h,
                                        std::optional<RendererOptions> given = std::nullopt) {
    RendererOptions options;
    if (std::getenv("FOVEA_REQUIRE_VALIDATION") != nullptr) {
        options.validation = Validation::required;
    }
    if (given) {
        options = *given;
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

// Draws `views` until paging settles: nothing asked for, loading, arriving
// or drawn coarser than wanted for a few frames. Returns the frames drawn
// (max_frames if it never settles).
int settle(Renderer& r, const std::vector<SceneView>& views, int max_frames = 600) {
    int quiet = 0;
    for (int f = 1; f <= max_frames; ++f) {
        r.draw_scene_frame(views);
        const TerrainStats& s = r.terrain_stats();
        quiet =
            s.requests == 0 && s.loading == 0 && s.uploads == 0 && s.misses == 0 ? quiet + 1 : 0;
        if (quiet >= 3) {
            return f;
        }
        if (s.loading > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));  // the paging thread's turn
        }
    }
    return max_frames;
}

// settle, until `models` placed models have arrived too.
void settle_models(Renderer& r, const std::vector<SceneView>& views, std::uint32_t models) {
    for (int i = 0; i < 100 && r.terrain_stats().models_loaded < models; ++i) {
        (void)settle(r, views, 60);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    (void)settle(r, views);
}

std::unique_ptr<TerrainDatabase> open_db(const fs::path& dir) {
    std::unique_ptr<TerrainDatabase> db;
    const auto r = TerrainDatabase::open(dir, db);
    EXPECT_TRUE(r.errors.empty()) << (r.errors.empty() ? "" : r.errors[0]);
    return db;
}

// The view: from `eye_height_m` above `origin` (ellipsoid heights here: no
// host, no geoid), turned by yaw and pitch, a frustum 60 degrees square
// unless told otherwise.
SceneView view_at(const Geodetic& origin, double eye_height_m, double pitch_deg,
                  double yaw_deg = 0.0, double half_fov_deg = 30.0) {
    ReferenceScene scene;
    scene.origin = origin;
    scene.eye_height_m = eye_height_m;
    ViewDesc v;
    v.pose.rotation = {yaw_deg, pitch_deg, 0.0};
    v.projection.frustum = {-half_fov_deg, half_fov_deg, half_fov_deg, -half_fov_deg};
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
    const fs::path dir = write_db("flat", s);
    constexpr std::uint32_t kSize = 256;
    RENDERER_OR_SKIP(r, kSize, kSize);
    const TerrainLoad load = r->load_terrain(open_db(dir));
    EXPECT_EQ(load.tiles, 1U);  // the root; the rest is paged
    EXPECT_TRUE(load.problems.empty()) << load.problems[0];
    // Looking straight down from 300 m, 40 m south-west of the corner.
    const DMat3 ned = ned_to_ecef(lat0, lon0);
    const Geodetic eye_at =
        ecef_to_geodetic(geodetic_to_ecef({lat0, lon0, 120.0}) + ned * DVec3(-40.0, -40.0, 0.0));
    const SceneView v = view_at({eye_at.lat_deg, eye_at.lon_deg, 120.0}, 300.0, -90.0);
    EXPECT_LT(settle(*r, {v}), 600);
    EXPECT_GT(r->terrain_tiles_drawn(), 1U);  // children paged in
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
    const fs::path dir = write_db("slope", s);
    constexpr std::uint32_t kSize = 256;
    RENDERER_OR_SKIP(r, kSize, kSize);
    (void)r->load_terrain(open_db(dir));
    const double h0 = rise(lat0, lon0);
    const DMat3 ned = ned_to_ecef(lat0, lon0);
    const Geodetic eye_at =
        ecef_to_geodetic(geodetic_to_ecef({lat0, lon0, h0}) + ned * DVec3(-200.0, 0.0, 0.0));
    const SceneView v = view_at({eye_at.lat_deg, eye_at.lon_deg, h0}, 200.0, -45.0);
    EXPECT_LT(settle(*r, {v}), 600);
    EXPECT_EQ(r->validation_messages(), 0U);
    const auto px = r->read_back_rgba(0);
    const glm::dvec2 at = project(v, {lat0, lon0, h0}, kSize, kSize);
    EXPECT_NEAR(at.x, kSize / 2.0, 2.0);  // in the middle: the eye looks at it
    EXPECT_NEAR(at.y, kSize / 2.0, 2.0);
    expect_corner_at(px, kSize, at, true);
    // Without its height (on the ellipsoid) it would show far off.
    EXPECT_GT(std::abs(project(v, {lat0, lon0, 0.0}, kSize, kSize).y - at.y), 20.0);
}

TEST(TerrainRender, PagingSkipsDamagedTilesAndStopsAtMaxLevel) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    const Spec s = quadrant_spec([](double, double) { return 120.0; }, &lat0, &lon0);
    const fs::path dir = write_db("damaged", s);
    const TileKey broken = children(s.root_key)[2];
    std::ofstream(dir / tile_path(broken), std::ios::binary | std::ios::trunc) << "not a tile";
    RENDERER_OR_SKIP(r, 64, 64);
    const TerrainLoad load = r->load_terrain(open_db(dir));
    EXPECT_EQ(load.tiles, 1U);
    EXPECT_TRUE(load.problems.empty());
    // The root can't split (a child failed to load), and is drawn whole.
    const std::vector<SceneView> v{view_at({lat0, lon0, 120.0}, 100.0, -90.0)};
    (void)settle(*r, v, 120);  // never quiet: the root misses its children for good
    EXPECT_EQ(r->terrain_stats().failures, 1U);
    EXPECT_EQ(r->terrain_stats().resident, 4U);  // the root and the three that loaded
    EXPECT_EQ(r->terrain_tiles_drawn(), 1U);
    EXPECT_EQ(r->validation_messages(), 0U);
    EXPECT_THROW((void)r->load_terrain(open_db(dir)), std::runtime_error);  // once only

    r.reset();  // one Renderer at a time
    RENDERER_OR_SKIP(r2, 64, 64);
    TerrainOptions only_roots;
    only_roots.max_level = 15;
    EXPECT_EQ(r2->load_terrain(open_db(dir), only_roots).tiles, 1U);
    (void)settle(*r2, v, 120);
    EXPECT_EQ(r2->terrain_stats().resident, 1U);  // nothing finer asked for
    EXPECT_EQ(r2->terrain_stats().failures, 0U);
    EXPECT_EQ(r2->terrain_stats().misses, 0U);  // nor missed: there is nothing to load
}

TEST(TerrainRender, ADamagedMiddleTileLeavesItsAncestorToCoverItsChildren) {
    // Three levels; one level-16 tile damaged. Its parent can't split and
    // covers everything; nothing below the damaged tile is asked for.
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = quadrant_spec([](double, double) { return 120.0; }, &lat0, &lon0);
    s.finest = 17;
    s.texels = 64;
    const fs::path dir = write_db("damaged_middle", s);
    const TileKey broken = children(s.root_key)[0];
    std::ofstream(dir / tile_path(broken), std::ios::binary | std::ios::trunc) << "not a tile";
    RENDERER_OR_SKIP(r, 64, 64);
    (void)r->load_terrain(open_db(dir));
    const GeoBounds b = tile_bounds(broken);
    (void)settle(*r,
                 {view_at({(b.south_deg + b.north_deg) / 2, (b.west_deg + b.east_deg) / 2, 120.0},
                          300.0, -90.0)},
                 120);
    EXPECT_EQ(r->terrain_tiles_drawn(), 1U);  // the root alone
    EXPECT_EQ(r->terrain_stats().resident, 4U);
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(TerrainRender, PagingKeepsUpAcrossAFlightWithNoHolesOrAllocation) {
    // Four levels, one colour everywhere; room for 56 tiles of 85: enough
    // for any one view, not for the whole flight.
    Spec s;
    s.finest = 18;
    s.texels = 32;
    s.root_key = tile_at(34.637, -86.775, s.root);
    s.height = [](double, double) { return 120.0; };
    s.colour = [](double, double) { return Rgb{220, 30, 220}; };
    const fs::path dir = write_db("flight", s);
    constexpr std::uint32_t kSize = 64;
    RenderOptionsNoValidation no_validation;  // the layer allocates on the calling thread
    auto r = make_renderer(kSize, kSize, no_validation.options);
    if (!r) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    TerrainOptions options;
    options.max_tiles = 56;
    (void)r->load_terrain(open_db(dir), options);
    // Straight down from 120 m, south to north across the root tile, well
    // inside it (the view covers about 140 m).
    const GeoBounds b = tile_bounds(s.root_key);
    const double lon = (b.west_deg + b.east_deg) / 2;
    const auto at = [&](double f) {
        const double lat = b.south_deg + (0.2 + 0.6 * f) * (b.north_deg - b.south_deg);
        return std::vector<SceneView>{view_at({lat, lon, 120.0}, 120.0, -90.0)};
    };
    std::uint32_t most_resident = 0;
    std::uint64_t allocations = 0;
    std::uint64_t uploads = 0;
    constexpr int kFrames = 400;
    for (int f = 0; f <= kFrames; ++f) {
        const auto views = at(static_cast<double>(f) / kFrames);
        {
            const test::AllocationCounter counter;
            r->draw_scene_frame(views);
            allocations += f > 10 ? counter.count() : 0;  // after the first frames' set-up
        }
        const TerrainStats& st = r->terrain_stats();
        most_resident = std::max(most_resident, st.resident + st.loading);
        uploads += st.uploads;
        ASSERT_GE(st.drawn, 1U);
        // Every pixel is terrain: some tile always covers the ground.
        const auto px = r->read_back_rgba(0);
        for (std::size_t i = 0; i < px.size(); i += 4) {
            ASSERT_GT(px[i], 150) << "frame " << f << ": a pixel that isn't terrain";
            ASSERT_LT(px[i + 1], 100) << "frame " << f;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));  // the paging thread's turn
    }
    EXPECT_EQ(allocations, 0U);
    EXPECT_LE(most_resident, 56U);
    EXPECT_GT(r->terrain_stats().evictions, 0U);
    EXPECT_GT(uploads, 56U);
    // Stopped at the end: paging catches up, and nothing is drawn coarser
    // than wanted.
    (void)settle(*r, at(1.0));
    EXPECT_EQ(r->terrain_stats().misses, 0U);
}

TEST(TerrainRender, TooSmallACacheMissesTilesWithoutThrashing) {
    // Room for 40 tiles where the view wants more: some tiles stay coarser
    // than wanted (misses), and nothing is evicted and loaded again and
    // again to chase them.
    Spec s;
    s.finest = 18;
    s.texels = 32;
    s.root_key = tile_at(34.637, -86.775, s.root);
    s.height = [](double, double) { return 120.0; };
    s.colour = [](double, double) { return Rgb{220, 30, 220}; };
    const fs::path dir = write_db("small_cache", s);
    RENDERER_OR_SKIP(r, 64, 64);
    TerrainOptions options;
    options.max_tiles = 40;
    (void)r->load_terrain(open_db(dir), options);
    // Across the tile and stopped, as in the flight above: what was loaded
    // on the way competes with what the view now wants.
    const GeoBounds b = tile_bounds(s.root_key);
    const double lon = (b.west_deg + b.east_deg) / 2;
    const auto at = [&](double f) {
        const double lat = b.south_deg + (0.2 + 0.6 * f) * (b.north_deg - b.south_deg);
        return std::vector<SceneView>{view_at({lat, lon, 120.0}, 120.0, -90.0)};
    };
    for (int f = 0; f <= 200; ++f) {
        r->draw_scene_frame(at(static_cast<double>(f) / 200.0));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const std::vector<SceneView> v = at(1.0);
    (void)settle(*r, v, 300);
    const std::uint64_t evictions = r->terrain_stats().evictions;
    std::uint64_t uploads = 0;
    for (int f = 0; f < 120; ++f) {
        r->draw_scene_frame(v);
        uploads += r->terrain_stats().uploads;
    }
    EXPECT_GT(r->terrain_stats().misses, 0U);
    EXPECT_EQ(r->terrain_stats().evictions, evictions);
    EXPECT_EQ(uploads, 0U);
    EXPECT_LE(r->terrain_stats().resident, 40U);
    EXPECT_EQ(r->validation_messages(), 0U);
}

// Hills and a pattern from 400 m: everything together, as a golden image
// (FOVEA_UPDATE_GOLDEN=1 rewrites it, on lavapipe, as test_golden.cpp).
// A box model: `half_x` either side (glTF's x, its left), `half_z` front and
// back, `height` tall; one plain colour (linear).
std::vector<std::uint8_t> box_glb(float half_x, float height, float half_z,
                                  std::array<float, 4> colour, const std::string& node = "") {
    test::GlbSpec g = test::glb_box(half_x, height, half_z);
    g.node = node;
    g.base_colour = colour;
    g.extras = R"({"fovea_material": 1})";
    return test::make_glb(g);
}

// Red: the strongest channel by far.
bool is_red(const std::vector<std::uint8_t>& px, std::uint32_t w, int x, int y) {
    const std::size_t at = (static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 4;
    return px[at] > 120 && px[at + 1] < 60 && px[at + 2] < 60;
}

Spec flat_grey(double* lat0, double* lon0) {
    Spec s;
    s.root_key = tile_at(34.637, -86.775, s.root);
    const GeoBounds b = tile_bounds(s.root_key);
    *lat0 = b.south_deg + 0.37 * (b.north_deg - b.south_deg);
    *lon0 = b.west_deg + 0.41 * (b.east_deg - b.west_deg);
    s.height = [](double, double) { return 120.0; };
    s.colour = [](double, double) { return Rgb{60, 60, 60}; };
    return s;
}

TEST(TerrainRender, ModelsStandWhereTheirPlacementsPutThem) {
    // A red slab 40 m by 10 m and 2 m tall, its front (the 10 m side) to
    // the east (heading 90), and set 6 m to its left (north, facing east)
    // and 15 m forward (east) of its origin: seen from straight above, its
    // top shows where the view's matrix, in double, puts the top's corners
    // (a heading the wrong way round, or a mirrored frame, would put it
    // elsewhere).
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    s.placements = {{"slab", lat0, lon0, 120.0, 90.0F}};
    s.models = {{"slab", box_glb(20.0F, 2.0F, 5.0F, {1.0F, 0.0F, 0.0F, 1.0F},
                                 R"("translation":[6,0,15],)")}};
    const fs::path dir = write_db("placed", s);
    constexpr std::uint32_t kSize = 256;
    RENDERER_OR_SKIP(r, kSize, kSize);
    const TerrainLoad load = r->load_terrain(open_db(dir));
    EXPECT_EQ(load.placements, 1U);
    EXPECT_EQ(load.models, 1U);
    EXPECT_TRUE(load.problems.empty()) << load.problems[0];
    const SceneView v = view_at({lat0, lon0, 120.0}, 200.0, -90.0);
    settle_models(*r, {v}, 1);
    EXPECT_EQ(r->terrain_stats().models_loaded, 1U);
    EXPECT_EQ(r->terrain_stats().placements_drawn, 1U);
    EXPECT_TRUE(r->terrain_problems().empty()) << r->terrain_problems()[0];
    EXPECT_EQ(r->validation_messages(), 0U);
    const auto px = r->read_back_rgba(0);
    // The top's corners: 20 m either side of 6 m north, 5 m either side of
    // 15 m east, 2 m up.
    const DVec3 base = geodetic_to_ecef({lat0, lon0, 120.0});
    const DMat3 ned = ned_to_ecef(lat0, lon0);
    double x0 = 1e9;
    double x1 = -1e9;
    double y0 = 1e9;
    double y1 = -1e9;
    for (const double n : {6.0 - 20.0, 6.0 + 20.0}) {
        for (const double e : {15.0 - 5.0, 15.0 + 5.0}) {
            const glm::dvec2 at =
                project(v, ecef_to_geodetic(base + ned * DVec3(n, e, -2.0)), kSize, kSize);
            x0 = std::min(x0, at.x);
            x1 = std::max(x1, at.x);
            y0 = std::min(y0, at.y);
            y1 = std::max(y1, at.y);
        }
    }
    int rx0 = kSize;
    int rx1 = -1;
    int ry0 = kSize;
    int ry1 = -1;
    for (int y = 0; y < static_cast<int>(kSize); ++y) {
        for (int x = 0; x < static_cast<int>(kSize); ++x) {
            if (is_red(px, kSize, x, y)) {
                rx0 = std::min(rx0, x);
                rx1 = std::max(rx1, x);
                ry0 = std::min(ry0, y);
                ry1 = std::max(ry1, y);
            }
        }
    }
    ASSERT_GE(rx1, 0) << "no model drawn";
    // Pixels whose centres lie inside: each edge within a pixel.
    EXPECT_NEAR(rx0 + 0.5, x0, 1.0);
    EXPECT_NEAR(rx1 + 0.5, x1, 1.0);
    EXPECT_NEAR(ry0 + 0.5, y0, 1.0);
    EXPECT_NEAR(ry1 + 0.5, y1, 1.0);
    EXPECT_GT(y1 - y0, 3.0 * (x1 - x0));  // north-south, as the heading says
}

// How much of a pixel's colour is `to` rather than `from`, along the line
// between them: 0 for `from`, 1 for `to`,
// in linear light (the output is sRGB-encoded).
double mix_of(const std::vector<std::uint8_t>& px, std::uint32_t w, int x, int y,
              const std::array<double, 3>& from, const std::array<double, 3>& to) {
    const auto linear = [](double v) {
        v /= 255.0;
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    const std::size_t at = (static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 4;
    double along = 0.0;
    double length = 0.0;
    for (std::size_t c = 0; c < 3; ++c) {
        const double f = linear(from[c]);
        const double t = linear(to[c]);
        along += (linear(static_cast<double>(px[at + c])) - f) * (t - f);
        length += (t - f) * (t - f);
    }
    return along / length;
}

// The image of a database seen from `v`, settled with its `placements`
// models, drawn with `msaa` samples a pixel, lit by `sky` if given; empty
// with no Vulkan (the caller skips).
std::vector<std::uint8_t> draw_settled(const fs::path& dir, const SceneView& v, std::uint32_t size,
                                       std::uint32_t msaa, const SkyBodies* sky = nullptr,
                                       std::uint32_t placements = 1) {
    RendererOptions options;
    options.msaa = msaa;
    if (std::getenv("FOVEA_REQUIRE_VALIDATION") != nullptr) {
        options.validation = Validation::required;
    }
    auto r = make_renderer(size, size, options);
    if (!r) {
        return {};
    }
    if (sky != nullptr) {
        r->set_sky(*sky);
    }
    (void)r->load_terrain(open_db(dir));
    settle_models(*r, {v}, placements);
    EXPECT_EQ(r->terrain_stats().placements_drawn, placements);
    EXPECT_EQ(r->validation_messages(), 0U);
    return r->read_back_rgba(0);
}

// A pixel's colour, as doubles.
std::array<double, 3> colour_at(const std::vector<std::uint8_t>& px, std::uint32_t w, int x,
                                int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * w + static_cast<std::size_t>(x)) * 4;
    return {static_cast<double>(px[i]), static_cast<double>(px[i + 1]),
            static_cast<double>(px[i + 2])};
}

// The sun lights the walls that face it (milestone 6 steps 1 and 2): a
// white box at KHSV in June, seen from the east and from the west at 8 am,
// has its east wall brighter than its west. Just after sunset, with the sun
// 2 to 4 degrees under the horizon in the north-west, its north wall, which
// faces it, is no brighter than its south: only the sky lights them.
TEST(TerrainRender, TheSunLightsTheWallsThatFaceItAndNoneFromUnderTheHorizon) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    s.placements = {{"box", lat0, lon0, 120.0, 0.0F}};
    s.models = {{"box", box_glb(10.0F, 8.0F, 10.0F, {1.0F, 1.0F, 1.0F, 1.0F}, "")}};
    const fs::path dir = write_db("walls", s);
    constexpr std::uint32_t kSize = 128;
    const double m_lat = 1.0 / 111000.0;  // degrees of latitude a metre
    const double m_lon = m_lat / std::cos(glm::radians(lat0));
    // A wall's middle, seen from 40 m out, 20 m up, looking back at it.
    struct Side {
        double north_m;
        double east_m;
        double yaw_deg;
    };
    const auto wall = [&](const SkyBodies& sky, Side side) {
        const SceneView v =
            view_at({lat0 + side.north_m * 4.0 * m_lat, lon0 + side.east_m * 4.0 * m_lon, 120.0},
                    20.0, -25.0, side.yaw_deg);
        const auto px = draw_settled(dir, v, kSize, 1, &sky);
        if (px.empty()) {
            return -1.0;
        }
        const glm::dvec2 at = project(
            v, {lat0 + side.north_m * m_lat, lon0 + side.east_m * m_lon, 124.0}, kSize, kSize);
        return colour_at(px, kSize, static_cast<int>(at.x), static_cast<int>(at.y))[0];
    };
    const Side east{0.0, 10.0, 270.0};
    const Side west{0.0, -10.0, 90.0};
    const Side north{10.0, 0.0, 180.0};
    const Side south{-10.0, 0.0, 0.0};

    const SkyBodies morning = sky_bodies(julian_date(2026, 6, 21, 13.0 * 3600.0));
    const double e = wall(morning, east);
    if (e < 0.0) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    const double w = wall(morning, west);
    std::printf("8 am: east wall %.0f, west wall %.0f\n", e, w);
    EXPECT_GT(e, w + 30.0);

    // The first minute after sunset with the sun 2 to 4 degrees down.
    const DMat3 level = ned_to_ecef(lat0, lon0);
    SkyBodies dusk;
    double elevation = 0.0;
    for (int minute = 0; minute < 180; ++minute) {
        dusk = sky_bodies(julian_date(2026, 6, 22, minute * 60.0));
        elevation = glm::degrees(std::asin(-(glm::transpose(level) * dusk.sun).z));
        if (elevation < -2.0) {
            break;
        }
    }
    const DVec3 sun_ned = glm::transpose(level) * dusk.sun;
    ASSERT_LT(elevation, -2.0);
    ASSERT_GT(elevation, -4.0);
    ASSERT_GT(sun_ned.x, 0.0);  // in the north
    const double n = wall(dusk, north);
    const double so = wall(dusk, south);
    std::printf("dusk, sun %.1f degrees: north wall %.0f, south wall %.0f\n", elevation, n, so);
    EXPECT_GT(n, 10.0);  // lit by the sky, not black
    EXPECT_NEAR(n, so, 1.0);
}

// Lit windows (milestone 6 step 8, D-033): a dark box whose walls give off
// 40 cd/m² of warm light through an emissive texture lit over its upper
// half, beside the same box without, and a third far past the HDR target's
// range, seen from the east. At midnight the lit box's upper wall shows
// bright and warm, its lower wall as dark as the plain box's, and the third
// at its brightest, not a NaN's black; at noon the exposure hides the glow:
// the lit box's upper wall is within two levels of its lower.
TEST(TerrainRender, EmissiveMaterialsGlowAtNightAndNotByDay) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    const double m_lat = 1.0 / 111000.0;
    const double m_lon = m_lat / std::cos(glm::radians(lat0));
    s.placements = {{"lit", lat0, lon0, 120.0, 0.0F},
                    {"dark", lat0 - 30.0 * m_lat, lon0, 120.0, 0.0F},
                    {"blazing", lat0 + 30.0 * m_lat, lon0, 120.0, 0.0F}};
    const auto glowing_box = [](const char* strength, bool textured) {
        test::GlbSpec g = test::glb_box(10.0F, 8.0F, 10.0F);
        g.base_colour = {0.1F, 0.1F, 0.1F, 1.0F};
        g.extras = R"({"fovea_material": 1})";
        g.material = std::string(R"("emissiveFactor":[1,0.8,0.5],)") +
                     (textured ? R"("emissiveTexture":{"index":0},)" : "") +
                     R"("extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":)" +
                     strength + "}},";
        g.top = R"("extensionsUsed":["KHR_materials_emissive_strength"],)";
        if (textured) {
            // White over its upper half, black below (row 0 at the top).
            RgbaImage half{4, 4, std::vector<std::uint8_t>(64, 255)};
            std::fill(half.pixels.begin() + 32, half.pixels.end(), std::uint8_t{0});
            for (std::size_t i = 3; i < half.pixels.size(); i += 4) {
                half.pixels[i] = 255;
            }
            g.image = encode_png(half);
            g.image_is_base_colour = false;
        }
        return test::make_glb(g);
    };
    s.models = {{"lit", glowing_box("40", true)},
                {"dark", box_glb(10.0F, 8.0F, 10.0F, {0.1F, 0.1F, 0.1F, 1.0F}, "")},
                {"blazing", glowing_box("1e9", false)}};
    const fs::path dir = write_db("emissive", s);
    constexpr std::uint32_t kSize = 128;
    const SceneView v = view_at({lat0, lon0 + 100.0 * m_lon, 120.0}, 4.0, 0.0, 270.0);
    const auto wall = [&](const std::vector<std::uint8_t>& px, double north_m, double up_m) {
        const glm::dvec2 at =
            project(v, {lat0 + north_m * m_lat, lon0 + 10.0 * m_lon, 120.0 + up_m}, kSize, kSize);
        return colour_at(px, kSize, static_cast<int>(at.x), static_cast<int>(at.y));
    };
    // 05:00 UTC on 2026-06-21 is midnight at KHSV; 17:00 UTC noon.
    const SkyBodies night = sky_bodies(julian_date(2026, 6, 21, 5.0 * 3600.0));
    const auto dark_px = draw_settled(dir, v, kSize, 1, &night, 3);
    if (dark_px.empty()) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    const auto glowing = wall(dark_px, 0.0, 6.0);
    const auto below = wall(dark_px, 0.0, 2.0);
    const auto unlit = wall(dark_px, -30.0, 6.0);
    const auto blazing = wall(dark_px, 30.0, 4.0);
    std::printf(
        "midnight: lit wall %.0f %.0f %.0f, under it %.0f, plain %.0f, blazing %.0f %.0f "
        "%.0f\n",
        glowing[0], glowing[1], glowing[2], below[0], unlit[0], blazing[0], blazing[1], blazing[2]);
    // Bright and warm: the tone map keeps its colour (not clipped white).
    EXPECT_GT(glowing[0], 240.0);
    EXPECT_GT(glowing[0], glowing[2] + 30.0);
    EXPECT_GT(glowing[2], 120.0);
    EXPECT_NEAR(below[0], unlit[0], 2.0) << "the texture's dark half glows";
    EXPECT_LT(unlit[0], 60.0);
    EXPECT_GT(blazing[0], 250.0) << "overflow";

    const SkyBodies noon = sky_bodies(julian_date(2026, 6, 21, 17.0 * 3600.0));
    const auto day_px = draw_settled(dir, v, kSize, 1, &noon, 3);
    const auto lit_day = wall(day_px, 0.0, 6.0);
    const auto under_day = wall(day_px, 0.0, 2.0);
    std::printf("noon: lit wall %.0f %.0f %.0f, under it %.0f %.0f %.0f\n", lit_day[0], lit_day[1],
                lit_day[2], under_day[0], under_day[1], under_day[2]);
    for (std::size_t c = 0; c < 3; ++c) {
        EXPECT_NEAR(lit_day[c], under_day[c], 2.0) << "channel " << c;
    }
}

// The sky on the June solstice when the sun, in the morning, first stands
// `elevation_deg` above the level at (lat, lon): to the second.
SkyBodies sun_at_elevation(double lat, double lon, double elevation_deg) {
    const DVec3 up = ned_to_ecef(lat, lon) * DVec3(0.0, 0.0, -1.0);
    const auto elevation = [&](double seconds) {
        const SkyBodies sky = sky_bodies(julian_date(2026, 6, 21, seconds));
        return glm::degrees(std::asin(glm::dot(sky.sun, up)));
    };
    double lo = 10.0 * 3600.0;  // 5 am local, before sunrise
    double hi = 18.0 * 3600.0;  // noon local
    for (int i = 0; i < 40; ++i) {
        const double mid = (lo + hi) / 2.0;
        (elevation(mid) < elevation_deg ? lo : hi) = mid;
    }
    return sky_bodies(julian_date(2026, 6, 21, hi));
}

// Shadows (milestone 6 step 5, D-032): a white post 20 m tall and 1 m
// square on flat ground, seen from straight above, casts its shadow away
// from the sun, as long as its height over the tangent of the sun's
// elevation (and its footprint's reach that way): its tip within two of the
// shadow map's texels and a pixel of where the geometry puts it, at 30 and
// 60 degrees. The sunlit ground beside it is even (no acne).
TEST(TerrainRender, APostsShadowFallsWhereTheSunPutsIt) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    constexpr double kHeight = 20.0;
    s.placements = {{"post", lat0, lon0, 120.0, 0.0F}};
    s.models = {
        {"post", box_glb(0.5F, static_cast<float>(kHeight), 0.5F, {1.0F, 1.0F, 1.0F, 1.0F}, "")}};
    const fs::path dir = write_db("shadow", s);
    constexpr std::uint32_t kSize = 256;
    // 300 m up, 12 degrees across (about a quarter of a metre a pixel), over
    // the middle of the shadow.
    const double pixel_m = 2.0 * 300.0 * std::tan(glm::radians(6.0)) / kSize;
    const DMat3 ned = ned_to_ecef(lat0, lon0);
    const DVec3 base = geodetic_to_ecef({lat0, lon0, 120.0});
    for (const double elevation : {30.0, 60.0}) {
        const SkyBodies sky = sun_at_elevation(lat0, lon0, elevation);
        // The sun in the level frame: the shadow runs the other way.
        const DVec3 sun = glm::transpose(ned) * sky.sun;
        const glm::dvec2 away = -glm::normalize(glm::dvec2(sun.x, sun.y));  // north, east
        const double length = kHeight * glm::length(glm::dvec2(sun.x, sun.y)) / -sun.z;
        // The footprint (1 m square, its sides north-south and east-west)
        // reaches half a metre times |north| + |east| further that way.
        const double tip = length + 0.5 * (std::abs(away.x) + std::abs(away.y));
        const Geodetic middle =
            ecef_to_geodetic(base + ned * DVec3(away.x * tip / 2.0, away.y * tip / 2.0, 0.0));
        const SceneView v = view_at(middle, 300.0, -90.0, 0.0, 6.0);
        const auto px = draw_settled(dir, v, kSize, 4, &sky);
        if (px.empty()) {
            GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
        }
        const auto brightness_at = [&](double along) {
            const DVec3 p = base + ned * DVec3(away.x * along, away.y * along, 0.0);
            const glm::dvec2 at = project(v, ecef_to_geodetic(p), kSize, kSize);
            const auto c = colour_at(px, kSize, static_cast<int>(at.x), static_cast<int>(at.y));
            return c[0] + c[1] + c[2];
        };
        const double dark = brightness_at(tip * 0.5);
        const double lit = brightness_at(tip + 8.0);
        double found = -1.0;
        for (double along = tip * 0.5; along < tip + 8.0; along += 0.02) {
            if (brightness_at(along) >= (dark + lit) / 2.0) {
                found = along;
                break;
            }
        }
        const ShadowCascades cascades = fit_shadow_cascades(v.eye_ecef, v.ecef_to_clip, sky.sun);
        std::uint32_t c = 0;
        while (c + 1 < kShadowCascades && cascades.far_m[c] < 300.0) {
            ++c;
        }
        const double tolerance = 2.0 * cascades.texel_m[c] + pixel_m;
        std::printf(
            "sun %.0f degrees: shadow %.2f m long, its tip at %.2f m (expected %.2f; "
            "%.0f dark, %.0f lit; tolerance %.2f m)\n",
            elevation, length, found, tip, dark, lit, tolerance);
        EXPECT_LT(dark, lit * 0.8);
        EXPECT_NEAR(found, tip, tolerance);

        // No acne: the sunlit ground beside the shadow, 5 to 15 m off its
        // line, along its length, even.
        double lo = 1e9;
        double hi = -1e9;
        const glm::dvec2 across(-away.y, away.x);
        for (double along = 0.0; along <= tip; along += 0.5) {
            const DVec3 p = base + ned * DVec3(away.x * along + across.x * 10.0,
                                               away.y * along + across.y * 10.0, 0.0);
            const glm::dvec2 at = project(v, ecef_to_geodetic(p), kSize, kSize);
            const auto rgb = colour_at(px, kSize, static_cast<int>(at.x), static_cast<int>(at.y));
            const double b = rgb[0] + rgb[1] + rgb[2];
            lo = std::min(lo, b);
            hi = std::max(hi, b);
        }
        EXPECT_LE(hi - lo, 3.0) << "sunlit ground uneven: acne";
    }
}

// The image `v` gives of the database in `dir` lit by `sky`, under the
// weather `layers` (step 6): settled, read back. Empty without Vulkan.
std::vector<std::uint8_t> draw_weather(const fs::path& dir, const SceneView& v, std::uint32_t size,
                                       const SkyBodies& sky, std::span<const WeatherLayer> layers) {
    auto r = make_renderer(size, size);
    if (!r) {
        return {};
    }
    r->set_sky(sky);
    r->set_weather(layers);
    (void)r->load_terrain(open_db(dir));
    settle_models(*r, {v}, 1);
    EXPECT_EQ(r->validation_messages(), 0U);
    return r->read_back_rgba(0);
}

double brightness(const std::vector<std::uint8_t>& px, std::uint32_t w, glm::dvec2 at) {
    const auto c = colour_at(px, w, static_cast<int>(at.x), static_cast<int>(at.y));
    return c[0] + c[1] + c[2];
}

// Rain and snow fall from an overcast (step 6; clouds aren't drawn yet,
// #73): under a band of rain the ground has only its grey light, no sun, so
// a post's shadow is gone; and the ground is darker than in the sun.
// Snow's overcast is thinner: its ground brighter than rain's.
TEST(TerrainRender, RainAndSnowTakeTheSunAndItsShadows) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    s.placements = {{"post", lat0, lon0, 120.0, 0.0F}};
    s.models = {{"post", box_glb(0.5F, 20.0F, 0.5F, {1.0F, 1.0F, 1.0F, 1.0F}, "")}};
    const fs::path dir = write_db("rain", s);
    constexpr std::uint32_t kSize = 128;
    const SkyBodies sky = sun_at_elevation(lat0, lon0, 40.0);
    const DMat3 ned = ned_to_ecef(lat0, lon0);
    const DVec3 base = geodetic_to_ecef({lat0, lon0, 120.0});
    const DVec3 sun = glm::transpose(ned) * sky.sun;
    const glm::dvec2 away = -glm::normalize(glm::dvec2(sun.x, sun.y));
    const double length = 20.0 * glm::length(glm::dvec2(sun.x, sun.y)) / -sun.z;
    const Geodetic middle =
        ecef_to_geodetic(base + ned * DVec3(away.x * length / 2.0, away.y * length / 2.0, 0.0));
    // 30 m up, so the rain's haze leaves the ground to be seen.
    const SceneView down = view_at(middle, 30.0, -90.0, 0.0, 20.0);
    const auto at = [&](double across) {
        const glm::dvec2 side(-away.y, away.x);
        return project(
            down, ecef_to_geodetic(base + ned * DVec3(away * (length * 0.5) + side * across, 0.0)),
            kSize, kSize);
    };
    const std::array<WeatherLayer, 1> rain = {
        WeatherLayer{WeatherKind::rain, 0.0, 1500.0, 1.0, 5000.0}};
    const auto sunny = draw_weather(dir, down, kSize, sky, {});
    if (sunny.empty()) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    const auto wet = draw_weather(dir, down, kSize, sky, rain);
    const double sunny_dark = brightness(sunny, kSize, at(0.0));
    const double sunny_lit = brightness(sunny, kSize, at(6.0));
    const double wet_dark = brightness(wet, kSize, at(0.0));
    const double wet_lit = brightness(wet, kSize, at(6.0));
    std::printf("sun: shadow %.0f, beside it %.0f; rain: %.0f, %.0f\n", sunny_dark, sunny_lit,
                wet_dark, wet_lit);
    EXPECT_LT(sunny_dark, sunny_lit * 0.8);
    EXPECT_NEAR(wet_dark, wet_lit, 3.0) << "a shadow in the rain";
    EXPECT_LT(wet_lit, sunny_lit) << "the rain's overcast no darker than the sun";
    // Snow falls from thinner cloud than rain: its ground lit brighter.
    const std::array<WeatherLayer, 1> snow = {
        WeatherLayer{WeatherKind::snow, 0.0, 1500.0, 1.0, 5000.0}};
    const double snowy_lit = brightness(draw_weather(dir, down, kSize, sky, snow), kSize, at(6.0));
    std::printf("snow: %.0f\n", snowy_lit);
    EXPECT_GT(snowy_lit, wet_lit * 1.3) << "snow's overcast no brighter than rain's";
}

// A fog layer hides what is in it and not what is above it (step 6): a
// white post 200 m tall, seen from 250 m up and 400 m away, through fog
// from below the ground to 60 m up, of 30 m visibility. Its foot is lost in
// the fog, as the ground beside it is; 100 m up, it shows as it does in
// clear air.
TEST(TerrainRender, AFogLayerHidesWhatIsInItAndNotWhatIsAbove) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    s.placements = {{"post", lat0, lon0, 120.0, 0.0F}};
    s.models = {{"post", box_glb(5.0F, 200.0F, 5.0F, {1.0F, 1.0F, 1.0F, 1.0F}, "")}};
    const fs::path dir = write_db("fog", s);
    constexpr std::uint32_t kSize = 128;
    const SkyBodies sky = sun_at_elevation(lat0, lon0, 40.0);
    const DMat3 ned = ned_to_ecef(lat0, lon0);
    const DVec3 base = geodetic_to_ecef({lat0, lon0, 120.0});
    const Geodetic eye = ecef_to_geodetic(base + ned * DVec3(-400.0, 0.0, 0.0));
    const SceneView v = view_at(eye, 250.0, -20.0, 0.0, 20.0);
    const std::array<WeatherLayer, 1> fog = {
        WeatherLayer{WeatherKind::fog, 70.0, 110.0, 1.0, 30.0}};
    const auto clear = draw_weather(dir, v, kSize, sky, {});
    if (clear.empty()) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    const auto foggy = draw_weather(dir, v, kSize, sky, fog);
    const auto on_post = [&](double up) {
        return project(v, ecef_to_geodetic(base + ned * DVec3(0.0, 0.0, -up)), kSize, kSize);
    };
    const glm::dvec2 beside =
        project(v, ecef_to_geodetic(base + ned * DVec3(0.0, 40.0, 0.0)), kSize, kSize);
    const double foot_clear = brightness(clear, kSize, on_post(10.0));
    const double foot_fog = brightness(foggy, kSize, on_post(10.0));
    const double ground_fog = brightness(foggy, kSize, beside);
    std::printf("the post's foot: clear %.0f, fog %.0f (the ground beside it %.0f)\n", foot_clear,
                foot_fog, ground_fog);
    EXPECT_GT(std::abs(foot_clear - brightness(clear, kSize, beside)), 60.0);
    EXPECT_NEAR(foot_fog, ground_fog, 6.0) << "the post's foot shows through the fog";
    for (const double up : {100.0, 150.0}) {
        const double a = brightness(clear, kSize, on_post(up));
        const double b = brightness(foggy, kSize, on_post(up));
        std::printf("%.0f m up the post: clear %.0f, fog %.0f\n", up, a, b);
        EXPECT_NEAR(a, b, 3.0) << "fog above its top";
    }
}

// Airport lights (milestone 6 step 8, D-033): at night, from a database's
// lights layer, each drawn where its point projects, at least a pixel,
// dimmer with distance and through fog, and the colour its side shows: a
// threshold light green to an aircraft landing over it, red beyond it
// (AC 150/5340-30J 2.3.2), and a PAPI unit white above its angle and red
// below.
std::vector<std::uint8_t> draw_lights(const fs::path& dir, const SceneView& v, std::uint32_t size,
                                      std::span<const WeatherLayer> layers = {},
                                      std::size_t lights = 0, std::uint32_t models = 0) {
    auto r = make_renderer(size, size);
    if (!r) {
        return {};
    }
    r->set_sky(sky_bodies(julian_date(2026, 6, 21, 5.0 * 3600.0)));  // midnight at KHSV
    r->set_weather(layers);
    const TerrainLoad load = r->load_terrain(open_db(dir));
    EXPECT_EQ(load.lights, lights);
    settle_models(*r, {v}, models);
    EXPECT_EQ(r->validation_messages(), 0U);
    return r->read_back_rgba(0);
}

// The brightest pixel within `reach` of `at`, and its place.
std::pair<glm::ivec2, double> brightest_near(const std::vector<std::uint8_t>& px, std::uint32_t w,
                                             glm::dvec2 at, int reach) {
    std::pair<glm::ivec2, double> best{{0, 0}, -1.0};
    for (int y = static_cast<int>(at.y) - reach; y <= static_cast<int>(at.y) + reach; ++y) {
        for (int x = static_cast<int>(at.x) - reach; x <= static_cast<int>(at.x) + reach; ++x) {
            if (x < 0 || y < 0 || x >= static_cast<int>(w) || y >= static_cast<int>(w)) {
                continue;
            }
            const double b = brightness(px, w, {x + 0.5, y + 0.5});
            if (b > best.second) {
                best = {{x, y}, b};
            }
        }
    }
    return best;
}

TEST(TerrainRender, AirportLightsShowAtNightWhereAndAsTheyShould) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    const DMat3 ned = ned_to_ecef(lat0, lon0);
    const DVec3 base = geodetic_to_ecef({lat0, lon0, 120.0});
    const auto at = [&](double north_m, double east_m) {
        return ecef_to_geodetic(base + ned * DVec3(north_m, east_m, 0.0));
    };
    using enum LightColour;
    const auto light = [&](double north_m, double east_m, LightType type,
                           std::array<LightColour, 2> colour, float papi = 0.0F,
                           float heading = 0.0F) {
        const Geodetic g = at(north_m, east_m);
        return TileLight{g.lat_deg, g.lon_deg, 120.0, heading, papi, type, colour};
    };
    // A runway heading north: a threshold light at the origin, white edge
    // lights 400 m to 3.2 km on, and a PAPI unit beside it at 3 degrees.
    // Then a threshold light of a runway heading north-east, and an edge
    // light behind a wall.
    s.lights = {light(0.0, 0.0, LightType::threshold, {green, red}),
                light(400.0, 0.0, LightType::edge, {white, white}),
                light(800.0, 0.0, LightType::edge, {white, white}),
                light(1600.0, 0.0, LightType::edge, {white, white}),
                light(3200.0, 0.0, LightType::edge, {white, white}),
                light(250.0, -30.0, LightType::papi, {red, none}, 3.0F),
                light(300.0, 300.0, LightType::threshold, {green, red}, 0.0F, 45.0F),
                light(400.0, 60.0, LightType::edge, {white, white})};
    // The wall: 20 m wide and 30 m tall, 150 m before that light on the
    // 702 m line from the eye below (at 250 m north, 9 m up): inside a
    // quarter of the way, outside the 48 m the light's depth is moved.
    const Geodetic wall = at(250.0, 60.0 * 550.0 / 700.0);
    s.placements = {{"wall", wall.lat_deg, wall.lon_deg, 120.0, 0.0F}};
    s.models = {{"wall", box_glb(10.0F, 30.0F, 1.0F, {0.2F, 0.2F, 0.2F, 1.0F}, "")}};
    const fs::path dir = write_db("lights", s);
    constexpr std::uint32_t kSize = 128;
    // Where a light shows: its point, lifted as an elevated fixture stands
    // (lights.slang), the brightest pixel within one of it, its colour,
    // and how many pixels around it are lit.
    struct Seen {
        glm::dvec2 expected;
        glm::ivec2 at;
        std::array<double, 3> colour;
        int lit = 0;
    };
    const auto seen = [&](const std::vector<std::uint8_t>& image, const SceneView& v,
                          std::size_t i) {
        const TileLight& l = s.lights[i];
        Seen r;
        r.expected = project(v, {l.lat_deg, l.lon_deg, l.height_m + 0.35}, kSize, kSize);
        r.at = brightest_near(image, kSize, r.expected, 1).first;
        r.colour = colour_at(image, kSize, r.at.x, r.at.y);
        for (int y = r.at.y - 4; y <= r.at.y + 4; ++y) {
            for (int x = r.at.x - 4; x <= r.at.x + 4; ++x) {
                r.lit += x >= 0 && y >= 0 && x < int{kSize} && y < int{kSize} &&
                         brightness(image, kSize, {x + 0.5, y + 0.5}) > 120.0;
            }
        }
        std::printf("light %zu: at %.1f, %.1f, brightest %d, %d (%.0f %.0f %.0f), %d lit\n", i,
                    r.expected.x, r.expected.y, r.at.x, r.at.y, r.colour[0], r.colour[1],
                    r.colour[2], r.lit);
        return r;
    };
    const auto sum = [](const std::array<double, 3>& c) { return c[0] + c[1] + c[2]; };

    // From 300 m south of the threshold, 40 m up, looking north: each
    // light within a pixel of its point, bright on the dark ground.
    const SceneView south = view_at(at(-300.0, 0.0), 40.0, -2.0, 0.0, 10.0);
    const auto px = draw_lights(dir, south, kSize, {}, s.lights.size(), 1);
    if (px.empty()) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    EXPECT_LT(brightness(px, kSize, {64.5, 120.5}), 30.0) << "the ground isn't dark";
    std::array<Seen, 6> clear;
    for (std::size_t i = 0; i < clear.size(); ++i) {
        clear[i] = seen(px, south, i);
        EXPECT_LE(std::abs(clear[i].at.x + 0.5 - clear[i].expected.x), 1.0) << i;
        EXPECT_LE(std::abs(clear[i].at.y + 0.5 - clear[i].expected.y), 1.0) << i;
        EXPECT_GT(sum(clear[i].colour), 400.0) << i;
    }
    // The threshold light green, landing over it (AC 150/5340-30J 2.3.2);
    // the PAPI unit white, 4.2 degrees above it.
    EXPECT_GT(clear[0].colour[1], clear[0].colour[0] + 100.0);
    EXPECT_GT(std::min({clear[5].colour[0], clear[5].colour[1], clear[5].colour[2]}), 200.0);

    // The wall hides the light behind it: no brighter than the wall.
    const Seen hidden = seen(px, south, 7);
    EXPECT_LT(sum(hidden.colour), 60.0) << "a light shows through the wall";

    // The north-east runway's threshold light, 26.6 degrees right of
    // north from the eye, 18 degrees off its runway's line on the
    // approach side: green. (An axis turned the wrong way, north-west,
    // would put the eye 72 degrees off it, all but outside its beam.)
    const SceneView toward = view_at(at(-300.0, 0.0), 40.0, -2.0, 26.57, 10.0);
    const Seen angled = seen(draw_lights(dir, toward, kSize, {}, s.lights.size(), 1), toward, 6);
    EXPECT_GT(angled.colour[1], angled.colour[0] + 100.0);
    EXPECT_GT(sum(angled.colour), 300.0);

    // Beyond the threshold, looking back south: red.
    const SceneView north = view_at(at(300.0, 0.0), 40.0, -5.0, 180.0, 10.0);
    const Seen back = seen(draw_lights(dir, north, kSize, {}, s.lights.size(), 1), north, 0);
    EXPECT_GT(back.colour[0], back.colour[1] + 100.0);
    // 8 m up, 0.8 degrees above the PAPI unit: red.
    const SceneView low = view_at(at(-300.0, 0.0), 8.0, 0.0, 0.0, 10.0);
    const Seen under = seen(draw_lights(dir, low, kSize, {}, s.lights.size(), 1), low, 5);
    EXPECT_GT(under.colour[0], under.colour[1] + 100.0);

    // 30 m from the threshold light: several pixels, where far lights
    // are one or two.
    const SceneView near = view_at(at(-30.0, 0.0), 3.0, -3.0, 0.0, 10.0);
    const Seen close = seen(draw_lights(dir, near, kSize, {}, s.lights.size(), 1), near, 0);
    EXPECT_GE(close.lit, 9);
    EXPECT_LE(clear[4].lit, 2);

    // Through fog from 100 to 160 m up, 800 m visibility: the farther a
    // light, the dimmer; the farthest, 3.5 km off, lost.
    const std::array<WeatherLayer, 1> fog = {
        WeatherLayer{WeatherKind::fog, 100.0, 60.0, 1.0, 800.0}};
    const auto foggy = draw_lights(dir, south, kSize, fog, s.lights.size(), 1);
    std::array<double, 5> through{};
    for (std::size_t i = 1; i <= 4; ++i) {
        through[i] = sum(seen(foggy, south, i).colour);
    }
    EXPECT_LT(through[3], sum(clear[3].colour) - 100.0);
    EXPECT_GE(through[2], through[3]);
    EXPECT_GT(through[3], through[4] + 200.0);
    EXPECT_LT(through[4], 60.0);
    if (const char* dump = std::getenv("FOVEA_DUMP_LIGHTS")) {
        (void)write_bytes_atomically(dump, encode_png(RgbaImage{kSize, kSize, px}));
    }
}

// Shadows across views (step 5): three views of terrain with a model, the
// sun up, drawn under validation (synchronization included): each view's
// maps are drawn before its scene, and every view's are readable before
// any is bound.
TEST(TerrainRender, SeveralViewsDrawTheirShadowsCleanly) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    s.placements = {{"post", lat0, lon0, 120.0, 0.0F}};
    s.models = {{"post", box_glb(0.5F, 20.0F, 0.5F, {1.0F, 1.0F, 1.0F, 1.0F}, "")}};
    const fs::path dir = write_db("shadow_views", s);
    RendererOptions options;
    if (std::getenv("FOVEA_REQUIRE_VALIDATION") != nullptr) {
        options.validation = Validation::required;
    }
    std::unique_ptr<Renderer> r;
    try {
        r = Renderer::offscreen(std::vector<OffscreenOutput>(3, {96, 96}), options);
    } catch (const std::runtime_error& e) {
        if (std::getenv("FOVEA_REQUIRE_VULKAN") != nullptr) {
            ADD_FAILURE() << "no Vulkan: " << e.what();
        }
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    r->set_sky(sun_at_elevation(lat0, lon0, 40.0));
    (void)r->load_terrain(open_db(dir));
    // 90 m south of the post, the first view facing it.
    const Geodetic eye{lat0 - 0.0008, lon0, 120.0};
    const std::vector<SceneView> views = {view_at(eye, 60.0, -30.0, 0.0),
                                          view_at(eye, 60.0, -30.0, 120.0),
                                          view_at(eye, 60.0, -30.0, 240.0)};
    settle_models(*r, views, 1);
    EXPECT_GT(r->terrain_stats().placements_drawn, 0U);
    EXPECT_EQ(r->terrain_stats().placements_dropped, 0U);
    EXPECT_EQ(r->validation_messages(), 0U);
}

// The terrain casts with the sun low (D-032): a hill 60 m high, the sun 10
// degrees up in the east, shades the ground behind it (west) and not the
// ground before it.
TEST(TerrainRender, AHillShadesTheGroundBehindItInALowSun) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    const double clat = lat0;
    const double clon = lon0;
    s.height = [clat, clon](double lat, double lon) {
        const double n = (lat - clat) * 111000.0;
        const double e = (lon - clon) * 91000.0;
        return 120.0 + 60.0 * std::exp(-(n * n + e * e) / (2.0 * 40.0 * 40.0));
    };
    s.placements = {{"post", lat0 + 0.0012, lon0, 120.0, 0.0F}};  // draw_settled wants one
    s.models = {{"post", box_glb(0.5F, 1.0F, 0.5F, {1.0F, 1.0F, 1.0F, 1.0F}, "")}};
    const fs::path dir = write_db("hill_shadow", s);
    constexpr std::uint32_t kSize = 256;
    const SceneView v = view_at({lat0, lon0, 120.0}, 1200.0, -90.0, 0.0, 20.0);
    const SkyBodies sky = sun_at_elevation(lat0, lon0, 10.0);
    const auto px = draw_settled(dir, v, kSize, 4, &sky);
    if (px.empty()) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    const DMat3 ned = ned_to_ecef(lat0, lon0);
    const DVec3 sun = glm::transpose(ned) * sky.sun;
    const glm::dvec2 toward = glm::normalize(glm::dvec2(sun.x, sun.y));
    // 130 m from the hill's top, before it and behind it, where the ground
    // is flat again (the hill is 40 m wide there, its shadow reaching about
    // 340 m) and inside the database (about 200 m to its west edge).
    const auto brightness_at = [&](double along) {
        const double n = toward.x * along;
        const double e = toward.y * along;
        const Geodetic g{lat0 + n / 111000.0, lon0 + e / 91000.0, 120.0};
        const glm::dvec2 at = project(v, g, kSize, kSize);
        const auto c = colour_at(px, kSize, static_cast<int>(at.x), static_cast<int>(at.y));
        return c[0] + c[1] + c[2];
    };
    const double before = brightness_at(130.0);
    const double behind = brightness_at(-130.0);
    std::printf("a hill in a low sun: ground before it %.0f, behind it %.0f\n", before, behind);
    EXPECT_LT(behind, before * 0.85);
}

// Multisampling (milestone 5 step 1, D-027): seen from straight above, a red
// slab turned 10 degrees leaves pixels along its long sides between its red
// and the grey ground, the more red the more of the pixel it covers; one
// sample a pixel leaves only the two. (Not equal to the cover: the tone
// curve, applied after the resolve, isn't straight.)
TEST(TerrainRender, MultisamplingBlendsAnEdgeByHowMuchOfThePixelItCovers) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    s.placements = {{"slab", lat0, lon0, 120.0, 10.0F}};
    s.models = {{"slab", box_glb(4.0F, 2.0F, 30.0F, {1.0F, 0.0F, 0.0F, 1.0F}, "")}};
    const fs::path dir = write_db("edge", s);
    constexpr std::uint32_t kSize = 256;
    const SceneView v = view_at({lat0, lon0, 120.0}, 200.0, -90.0);
    // The top's long sides, 4 m either side of its middle, as lines in the
    // image (straight: the top is level, seen from above).
    const DVec3 base = geodetic_to_ecef({lat0, lon0, 120.0});
    const DMat3 ned = ned_to_ecef(lat0, lon0);
    const double heading = glm::radians(10.0);
    const auto at = [&](double along, double across) {
        const DVec3 n(along * std::cos(heading) - across * std::sin(heading),
                      along * std::sin(heading) + across * std::cos(heading), -2.0);
        return project(v, ecef_to_geodetic(base + ned * n), kSize, kSize);
    };
    struct Side {
        glm::dvec2 a;
        glm::dvec2 b;
        bool red_right;  // the slab to the side's right (greater x)
    };
    const std::array<Side, 2> sides = {Side{at(-20.0, -4.0), at(20.0, -4.0), true},
                                       Side{at(-20.0, 4.0), at(20.0, 4.0), false}};
    const int mid = static_cast<int>(kSize / 2);
    for (const std::uint32_t msaa : {1U, 4U}) {
        const auto px = draw_settled(dir, v, kSize, msaa);
        if (px.empty()) {
            GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
        }
        const auto colour = [&](int x, int y) { return colour_at(px, kSize, x, y); };
        const auto grey = colour(4, mid);
        const auto red = colour(mid, mid);
        // Each pixel across the sides along 60 rows: how much of it the
        // slab covers (by where the side crosses the row's middle; the side
        // leans less than a fifth of a pixel a row), and how red it is.
        std::vector<std::pair<double, double>> partial;
        int between = 0;
        for (int y = mid - 30; y < mid + 30; ++y) {
            for (const Side& side : sides) {
                const double t = (y + 0.5 - side.a.y) / (side.b.y - side.a.y);
                const double edge = side.a.x + t * (side.b.x - side.a.x);
                const int e = static_cast<int>(std::floor(edge));
                for (int x = e - 1; x <= e + 1; ++x) {
                    const double right = std::clamp(x + 1 - edge, 0.0, 1.0);
                    const double cover = side.red_right ? right : 1.0 - right;
                    const double m = mix_of(px, kSize, x, y, grey, red);
                    if (m > 0.05 && m < 0.95) {
                        ++between;
                    }
                    if (cover > 0.0 && cover < 1.0) {
                        partial.emplace_back(cover, m);
                    }
                }
            }
        }
        if (msaa == 1) {
            EXPECT_EQ(between, 0) << "pixels between the two colours with one sample";
            continue;
        }
        // Many in between, as red as they're covered.
        EXPECT_GT(between, 60);
        ASSERT_GT(partial.size(), 100U);
        double mc = 0.0;
        double mm = 0.0;
        for (const auto& [c, m] : partial) {
            mc += c;
            mm += m;
        }
        mc /= static_cast<double>(partial.size());
        mm /= static_cast<double>(partial.size());
        double cov = 0.0;
        double vc = 0.0;
        double vm = 0.0;
        for (const auto& [c, m] : partial) {
            cov += (c - mc) * (m - mm);
            vc += (c - mc) * (c - mc);
            vm += (m - mm) * (m - mm);
        }
        const double correlation = cov / std::sqrt(vc * vm);
        std::printf("msaa %u: %d pixels between, cover and redness correlate %.3f\n", msaa, between,
                    correlation);
        // 0.92 on lavapipe: four samples cover a pixel in quarters.
        EXPECT_GT(correlation, 0.85);
    }
}

// Alpha to coverage (milestone 5 step 2): a box's top textured with alpha
// rising evenly across it, its material masked at 0.3, is cut where alpha
// is 0.3, off its middle (where plain alpha to coverage, or a cutoff of 0.5,
// would put it). At 4 samples the cut's pixels are partly covered, between the
// red top and the grey ground; at 1 they are one or the other; either way
// the cut is where the cutoff puts it, within a pixel.
TEST(TerrainRender, AlphaToCoverageSmoothsAMaskedEdgeWhereTheCutoffPutsIt) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    s.placements = {{"cut", lat0, lon0, 120.0, 10.0F}};
    test::GlbSpec g = test::glb_box(20.0F, 2.0F, 20.0F);
    g.base_colour = {1.0F, 0.0F, 0.0F, 1.0F};
    g.extras = R"({"fovea_material": 1})";
    g.material = R"("alphaMode":"MASK","alphaCutoff":0.3,)";
    RgbaImage ramp{64, 4, std::vector<std::uint8_t>(64 * 4 * 4, 255)};
    for (std::uint32_t y = 0; y < ramp.height; ++y) {
        for (std::uint32_t x = 0; x < ramp.width; ++x) {
            ramp.pixels[(y * ramp.width + x) * 4 + 3] =
                static_cast<std::uint8_t>(std::lround(255.0 * x / (ramp.width - 1)));
        }
    }
    g.image = encode_png(ramp);
    s.models = {{"cut", test::make_glb(g)}};
    const fs::path dir = write_db("cut", s);
    constexpr std::uint32_t kSize = 256;
    const SceneView v = view_at({lat0, lon0, 120.0}, 200.0, -90.0);
    // The cut, as a line in the image along the box's depth: where alpha,
    // i / 63 at texel i's centre (u = (i + 0.5) / 64), is 0.3. The texture's
    // u runs along the model's +x, its left: east at -x.
    const double u = (0.3 * 63.0 + 0.5) / 64.0;
    const double across = -(u - 0.5) * 40.0;
    const DVec3 base = geodetic_to_ecef({lat0, lon0, 120.0});
    const DMat3 ned = ned_to_ecef(lat0, lon0);
    const double heading = glm::radians(10.0);
    const auto at = [&](double along) {
        const DVec3 n(along * std::cos(heading) - across * std::sin(heading),
                      along * std::sin(heading) + across * std::cos(heading), -2.0);
        return project(v, ecef_to_geodetic(base + ned * n), kSize, kSize);
    };
    const glm::dvec2 a = at(-15.0);
    const glm::dvec2 b = at(15.0);
    const int mid = static_cast<int>(kSize / 2);
    for (const std::uint32_t msaa : {1U, 4U}) {
        const auto px = draw_settled(dir, v, kSize, msaa);
        if (px.empty()) {
            GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
        }
        const auto colour = [&](int x, int y) { return colour_at(px, kSize, x, y); };
        const auto grey = colour(4, mid);
        int between = 0;
        double worst = 0.0;
        for (int y = mid - 15; y < mid + 15; ++y) {
            const double t = (y + 0.5 - a.y) / (b.y - a.y);
            const double cut = a.x + t * (b.x - a.x);
            const int c = static_cast<int>(std::floor(cut));
            // Red on one side, grey on the other.
            const auto left = colour(c - 6, y);
            const auto right = colour(c + 6, y);
            const bool red_left = left[0] > right[0];
            const auto red = red_left ? left : right;
            ASSERT_GT(red[0], grey[0] + 50.0) << "no red top at row " << y;
            // Where the redness crosses a half, between pixel centres.
            double crossing = -1.0;
            double previous = 0.0;
            for (int x = c - 5; x <= c + 5; ++x) {
                const double m0 = mix_of(px, kSize, x, y, grey, red);
                const double m = red_left ? 1.0 - m0 : m0;  // rising left to right
                if (m > 0.05 && m < 0.95) {
                    ++between;
                }
                if (x > c - 5 && crossing < 0.0 && previous < 0.5 && m >= 0.5) {
                    crossing = x - 0.5 + (0.5 - previous) / (m - previous);
                }
                previous = m;
            }
            ASSERT_GE(crossing, 0.0) << "no cut at row " << y;
            worst = std::max(worst, std::abs(crossing - cut));
        }
        std::printf("msaa %u: %d pixels between, the cut at most %.2f pixels from the cutoff's\n",
                    msaa, between, worst);
        EXPECT_LT(worst, 1.0);
        if (msaa == 1) {
            EXPECT_EQ(between, 0) << "a cut-out's edge between the two with one sample";
        } else {
            // About a pixel wide: a row's edge in one or two pixels.
            EXPECT_GE(between, 20) << "a cut-out's edge as hard as with one sample";
            EXPECT_LE(between, 60) << "a cut-out's edge wider than a pixel or two";
        }
    }
}

// glTF's rule, multisampled too: a masked material whose alpha, the same
// everywhere, is its cutoff is opaque, not half covered.
TEST(TerrainRender, AMaskAtItsCutoffIsOpaqueAtEverySampleCount) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    s.placements = {{"flat", lat0, lon0, 120.0, 0.0F}};
    test::GlbSpec g = test::glb_box(20.0F, 2.0F, 20.0F);
    g.base_colour = {1.0F, 0.0F, 0.0F, 0.5F};
    g.extras = R"({"fovea_material": 1})";
    g.material = R"("alphaMode":"MASK","alphaCutoff":0.5,)";
    s.models = {{"flat", test::make_glb(g)}};
    const fs::path dir = write_db("at-cutoff", s);
    constexpr std::uint32_t kSize = 64;
    const SceneView v = view_at({lat0, lon0, 120.0}, 200.0, -90.0);
    std::array<std::array<double, 3>, 2> middles{};
    for (const std::uint32_t msaa : {1U, 4U}) {
        const auto px = draw_settled(dir, v, kSize, msaa);
        if (px.empty()) {
            GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
        }
        middles[msaa == 1 ? 0 : 1] = colour_at(px, kSize, kSize / 2, kSize / 2);
    }
    EXPECT_GT(middles[0][0], 120.0) << "not drawn with one sample";
    for (std::size_t c = 0; c < 3; ++c) {
        EXPECT_NEAR(middles[1][c], middles[0][c], 2.0) << "channel " << c;
    }
}

TEST(TerrainRender, AModelThatCannotLoadIsReportedAndTheRestDrawn) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    s.placements = {{"slab", lat0, lon0, 120.0, 0.0F},
                    {"ghost", lat0, lon0 + 0.0003, 120.0, 0.0F},
                    {"broken", lat0, lon0 - 0.0003, 120.0, 0.0F}};
    s.models = {{"slab", box_glb(5.0F, 2.0F, 5.0F, {1.0F, 0.0F, 0.0F, 1.0F})},
                {"broken", {'g', 'l', 'T', 'F', 2, 0, 0, 0}}};
    const fs::path dir = write_db("unloadable", s);
    constexpr std::uint32_t kSize = 128;
    RENDERER_OR_SKIP(r, kSize, kSize);
    const TerrainLoad load = r->load_terrain(open_db(dir));
    EXPECT_EQ(load.models, 3U);
    const SceneView v = view_at({lat0, lon0, 120.0}, 200.0, -90.0);
    settle_models(*r, {v}, 1);
    for (int i = 0; i < 20; ++i) {
        r->draw_scene_frame(std::vector<SceneView>{v});  // the failures come in too
    }
    EXPECT_EQ(r->terrain_stats().models_loaded, 1U);
    EXPECT_EQ(r->terrain_stats().placements_drawn, 1U);
    const auto problems = r->terrain_problems();
    ASSERT_EQ(problems.size(), 2U);
    EXPECT_NE(problems[0].find("broken"), std::string::npos) << problems[0];  // name order
    EXPECT_NE(problems[1].find("ghost"), std::string::npos) << problems[1];
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(TerrainRender, ModelsThatCannotBeSetUpLeaveTheTerrain) {
    // More placements than the renderer takes: reported, and the terrain
    // drawn without them.
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    s.placements.assign(4097, Placement{"a", lat0, lon0, 120.0, 0.0F});
    s.models = {{"a", box_glb(3.0F, 4.0F, 2.0F, {1.0F, 0.0F, 0.0F, 1.0F})}};
    const fs::path dir = write_db("too_many", s);
    RENDERER_OR_SKIP(r, 64, 64);
    const TerrainLoad load = r->load_terrain(open_db(dir));
    EXPECT_EQ(load.tiles, 1U);
    EXPECT_EQ(load.models, 0U);
    ASSERT_EQ(load.problems.size(), 1U);
    EXPECT_NE(load.problems[0].find("4097 placements"), std::string::npos) << load.problems[0];
    const SceneView v = view_at({lat0, lon0, 120.0}, 200.0, -90.0);
    (void)settle(*r, {v});
    EXPECT_GE(r->terrain_tiles_drawn(), 1U);
    EXPECT_EQ(r->terrain_stats().placements_drawn, 0U);
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(TerrainRender, AFrameTakesAFullLoadOfTilesAndModelsTogether) {
    // Two views asking for many tiles at once, and four models, all ready
    // before the next frame: it takes 16 tiles and models together, and
    // submits them all (the submit once had room for 16 uploads only).
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    s.finest = 18;
    s.texels = 32;
    // Big textures (1024 x 1024 of noise): each model takes a while to
    // decode, so models are still arriving when the tiles do.
    std::mt19937 rng(5);
    RgbaImage noise{1024, 1024, std::vector<std::uint8_t>(std::size_t{1024} * 1024 * 4)};
    for (auto& v : noise.pixels) {
        v = static_cast<std::uint8_t>(rng());
    }
    const auto texture = encode_png(noise);
    for (int i = 0; i < 4; ++i) {
        const std::string name = "m" + std::to_string(i);
        s.placements.push_back({name, lat0 + 0.0002 * i, lon0, 120.0, 0.0F});
        test::GlbSpec g = test::glb_box(2.0F + static_cast<float>(i), 3.0F, 2.0F);
        g.image = texture;
        g.extras = R"({"fovea_material": 1})";
        s.models.emplace_back(name, test::make_glb(g));
    }
    const fs::path dir = write_db("full_load", s);
    RendererOptions options;
    if (std::getenv("FOVEA_REQUIRE_VALIDATION") != nullptr) {
        options.validation = Validation::required;
    }
    std::unique_ptr<Renderer> r;
    try {
        r = Renderer::offscreen(std::vector<OffscreenOutput>{{128, 128}, {128, 128}}, options);
    } catch (const std::runtime_error& e) {
        if (std::getenv("FOVEA_REQUIRE_VULKAN") != nullptr) {
            ADD_FAILURE() << "no Vulkan: " << e.what();
        }
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    (void)r->load_terrain(open_db(dir));
    const GeoBounds b = tile_bounds(s.root_key);
    const double lat = (b.south_deg + b.north_deg) / 2;
    const double lon = (b.west_deg + b.east_deg) / 2;
    const std::vector<SceneView> views{view_at({lat, lon, 120.0}, 60.0, -90.0),
                                       view_at({lat, lon, 120.0}, 60.0, -30.0, 90.0)};
    r->draw_scene_frame(views);  // asks for children: 16 a view
    bool together = false;
    for (int f = 0; f < 100 && !together; ++f) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        const std::uint32_t before = r->terrain_stats().models_loaded;
        r->draw_scene_frame(views);
        const TerrainStats& st = r->terrain_stats();
        together = st.uploads == 16 && st.models_loaded > before;
    }
    if (!together) {
        // Timing, not a fault: the models came in before or after the
        // tiles here. The renderer refuses more than it has room for, so
        // the overrun can't come back unseen either way.
        GTEST_SKIP() << "no frame happened to take 16 tiles and a model together";
    }
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(TerrainRender, DrawingModelsAllocatesNothing) {
    double lat0 = 0.0;
    double lon0 = 0.0;
    Spec s = flat_grey(&lat0, &lon0);
    for (int i = 0; i < 50; ++i) {
        s.placements.push_back({i % 2 == 0 ? "a" : "b", lat0 + 0.0001 * (i % 7),
                                lon0 + 0.0001 * (i / 7), 120.0, static_cast<float>(i * 7)});
    }
    s.models = {{"a", box_glb(3.0F, 4.0F, 2.0F, {1.0F, 0.0F, 0.0F, 1.0F})},
                {"b", box_glb(2.0F, 6.0F, 2.0F, {0.0F, 0.0F, 1.0F, 1.0F})}};
    const fs::path dir = write_db("many", s);
    constexpr std::uint32_t kSize = 64;
    RenderOptionsNoValidation no_validation;  // the layer allocates on the calling thread
    auto r = make_renderer(kSize, kSize, no_validation.options);
    if (!r) {
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    (void)r->load_terrain(open_db(dir));
    const SceneView v = view_at({lat0 + 0.0003, lon0 + 0.0003, 120.0}, 150.0, -60.0);
    settle_models(*r, {v}, 2);
    ASSERT_EQ(r->terrain_stats().models_loaded, 2U);
    const std::vector<SceneView> views{v};
    std::uint64_t allocations = 0;
    for (int f = 0; f < 60; ++f) {
        const test::AllocationCounter counter;
        r->draw_scene_frame(views);
        allocations += counter.count();
    }
    EXPECT_EQ(allocations, 0U);
    EXPECT_GT(r->terrain_stats().placements_drawn, 10U);
}

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
    const fs::path dir = write_db("hills", s);
    constexpr std::uint32_t kW = 320;
    constexpr std::uint32_t kH = 200;
    RENDERER_OR_SKIP(r, kW, kH);
    (void)r->load_terrain(open_db(dir));
    const SceneView v = view_at({b.south_deg + 0.0004, clon, 150.0}, 250.0, -30.0, 10.0);
    EXPECT_LT(settle(*r, {v}), 600);
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
    std::printf("golden terrain_hills    mean %.3f  max %3d  over 4: %.4f%%\n", d.mean_abs,
                int{d.max_abs}, d.fraction_over * 100.0);
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

TEST(TerrainRender, ModelsOnHillsMatchTheGoldenImage) {
    // The hills (smooth colours, as above) and boxes at several headings
    // standing on them, some textured, seen at an angle.
    Spec s;
    s.finest = 16;
    s.samples = 33;
    s.texels = 64;
    s.root_key = tile_at(34.637, -86.775, s.root);
    const GeoBounds b = tile_bounds(s.root_key);
    const double clat = (b.south_deg + b.north_deg) / 2;
    const double clon = (b.west_deg + b.east_deg) / 2;
    s.height = [clat, clon](double lat, double lon) {
        const double n = (lat - clat) * 111000.0;
        const double e = (lon - clon) * 91000.0;
        return 150.0 + 6.0 * std::sin(n / 60.0) * std::cos(e / 50.0);
    };
    s.colour = [](double lat, double lon) {
        const auto wave = [](double v) { return static_cast<std::uint8_t>(110.0 + 40.0 * v); };
        return Rgb{wave(std::sin(lat * 9000.0)), wave(std::cos(lon * 7000.0)), 90};
    };
    // A texture of smooth colour (GPUs filter differently: no hard edges).
    RgbaImage tex{32, 32, std::vector<std::uint8_t>(32 * 32 * 4)};
    for (std::uint32_t y = 0; y < 32; ++y) {
        for (std::uint32_t x = 0; x < 32; ++x) {
            std::uint8_t* p = &tex.pixels[(std::size_t{y} * 32 + x) * 4];
            p[0] = static_cast<std::uint8_t>(60 + 6 * x);
            p[1] = static_cast<std::uint8_t>(60 + 6 * y);
            p[2] = 200;
            p[3] = 255;
        }
    }
    test::GlbSpec textured = test::glb_box(8.0F, 12.0F, 5.0F);
    textured.image = encode_png(tex);
    textured.base_colour = {1.0F, 1.0F, 1.0F, 1.0F};
    textured.extras = R"({"fovea_material": 2})";
    s.models = {{"tower", box_glb(3.0F, 25.0F, 3.0F, {0.8F, 0.75F, 0.7F, 1.0F})},
                {"hangar", test::make_glb(textured)}};
    const std::array<std::array<double, 3>, 5> where = {
        {{0.0, 0.0, 0.0},
         {40.0, -30.0, 30.0},
         {-35.0, 25.0, 75.0},
         {70.0, 20.0, 135.0},
         {10.0, 60.0, 200.0}}};  // metres north and east of the centre, heading
    for (std::size_t i = 0; i < where.size(); ++i) {
        const double lat = clat + where[i][0] / 111000.0;
        const double lon = clon + where[i][1] / 91000.0;
        s.placements.push_back({i % 2 == 0 ? "tower" : "hangar", lat, lon, s.height(lat, lon) - 1.0,
                                static_cast<float>(where[i][2])});
    }
    const fs::path dir = write_db("models_on_hills", s);
    constexpr std::uint32_t kW = 320;
    constexpr std::uint32_t kH = 200;
    RENDERER_OR_SKIP(r, kW, kH);
    (void)r->load_terrain(open_db(dir));
    const SceneView v = view_at({clat - 0.0012, clon - 0.0002, 150.0}, 60.0, -15.0, 5.0);
    settle_models(*r, {v}, 2);
    ASSERT_EQ(r->terrain_stats().models_loaded, 2U);
    EXPECT_GE(r->terrain_stats().placements_drawn, 4U);
    EXPECT_EQ(r->validation_messages(), 0U);
    const RgbaImage image{kW, kH, r->read_back_rgba(0)};
    const fs::path file = FOVEA_SOURCE_DIR "/tests/data/golden/models_on_hills.png";
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
    std::printf("golden models_on_hills  mean %.3f  max %3d  over 4: %.4f%%\n", d.mean_abs,
                int{d.max_abs}, d.fraction_over * 100.0);
    if (d.fraction_over > 0.01) {
        const fs::path actual = fs::temp_directory_path() / "models_on_hills.actual.png";
        EXPECT_TRUE(write_png(actual, image));
        std::printf("wrote %s\n", actual.string().c_str());
    }
    EXPECT_LE(d.fraction_over, 0.01) << d.fraction_over * 100.0 << "% of pixels differ (mean "
                                     << d.mean_abs << ", max " << int{d.max_abs} << ")";
}

}  // namespace
}  // namespace fovea