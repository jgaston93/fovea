// Which terrain tiles a view picks (D-016; milestone 3 step 5): a real
// quadtree of tiles near KHSV, seen from 200 m up looking north and down.

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/geodesy.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/render/terrain_lod.hpp"
#include "fovea/terrain/tile_key.hpp"

namespace fovea {
namespace {

constexpr std::uint8_t kRoot = 14;
constexpr std::uint8_t kFinest = 18;
constexpr double kRootError = 4.0;  // halving with each level, 0 at the finest

struct Tree {
    std::vector<LodTile> tiles;
    std::vector<TileKey> keys;
    std::vector<std::uint32_t> roots;
};

// Every tile from 2 x 2 roots down to kFinest; spheres around each tile at
// height 0 (flat ground, as the test's eye sees it).
Tree make_tree() {
    Tree t;
    std::unordered_map<TileKey, std::uint32_t> index;
    const TileKey origin = tile_at(34.637, -86.775, kRoot);
    std::vector<TileKey> level;
    for (std::uint32_t dx = 0; dx < 2; ++dx) {
        for (std::uint32_t dy = 0; dy < 2; ++dy) {
            level.push_back({kRoot, origin.x + dx, origin.y + dy});
        }
    }
    while (!level.empty()) {
        std::vector<TileKey> next;
        for (const TileKey& k : level) {
            const GeoBounds b = tile_bounds(k);
            const DVec3 c = geodetic_to_ecef(
                {(b.south_deg + b.north_deg) / 2, (b.west_deg + b.east_deg) / 2, 0.0});
            double r = 0.0;
            for (const double lat : {b.south_deg, b.north_deg}) {
                for (const double lon : {b.west_deg, b.east_deg}) {
                    r = std::max(r, glm::length(geodetic_to_ecef({lat, lon, 0.0}) - c));
                }
            }
            LodTile tile;
            tile.centre_ecef = c;
            tile.radius_m = r + 1.0;
            tile.error_m = k.level == kFinest ? 0.0 : kRootError / std::exp2(k.level - kRoot);
            index.emplace(k, static_cast<std::uint32_t>(t.tiles.size()));
            t.tiles.push_back(tile);
            t.keys.push_back(k);
            if (k.level == kRoot) {
                t.roots.push_back(index.at(k));
            }
            if (k.level < kFinest) {
                for (const TileKey& child : children(k)) {
                    next.push_back(child);
                }
            }
        }
        level = std::move(next);
    }
    for (std::size_t i = 0; i < t.keys.size(); ++i) {
        if (t.keys[i].level < kFinest) {
            const auto kids = children(t.keys[i]);
            for (std::size_t c = 0; c < 4; ++c) {
                t.tiles[i].children[c] = index.at(kids[c]);
            }
        }
    }
    return t;
}

LodView view_from(double pitch_deg, double yaw_deg = 0.0) {
    ReferenceScene scene;
    scene.origin = {34.637, -86.775, 0.0};
    scene.eye_height_m = 200.0;
    ViewDesc v;
    v.pose.rotation = {yaw_deg, pitch_deg, 0.0};
    v.projection.frustum = {-30.0, 30.0, 20.0, -20.0};
    const SceneView sv = reference_scene_view(v, scene);
    LodView lv;
    lv.eye_ecef = sv.eye_ecef;
    lv.ecef_to_clip = sv.ecef_to_clip;
    lv.pixels_per_tangent = 1080.0 / static_cast<double>(sv.tan_height);
    lv.pixel_error = 1.0;
    return lv;
}

std::vector<std::uint32_t> select(const Tree& t, const LodView& v) {
    std::vector<std::uint32_t> stack(t.tiles.size());
    std::vector<std::uint32_t> out(t.tiles.size());
    out.resize(select_tiles(t.tiles, t.roots, v, stack, out));
    return out;
}

bool is_ancestor(const TileKey& a, const TileKey& d) {
    if (a.level >= d.level) {
        return false;
    }
    const int s = d.level - a.level;
    return (d.x >> s) == a.x && (d.y >> s) == a.y;
}

TEST(TerrainLod, ScreenErrorFallsWithDistance) {
    EXPECT_DOUBLE_EQ(screen_error_px(1.0, 1000.0, 1000.0), 1.0);
    EXPECT_DOUBLE_EQ(screen_error_px(1.0, 2000.0, 1000.0), 0.5);
    EXPECT_GT(screen_error_px(1.0, 0.0, 1000.0), 1e5);  // at (or in) the tile: always splits
}

TEST(TerrainLod, SpheresBehindOrBesideTheViewAreLeftOut) {
    const LodView v = view_from(0.0);  // level, looking north
    const DMat3 ned = ned_to_ecef(34.637, -86.775);
    const auto at = [&](double north, double east, double down) {
        return ned * DVec3(north, east, down);
    };
    EXPECT_TRUE(sphere_visible(v.ecef_to_clip, at(1000, 0, 0), 10.0));
    EXPECT_FALSE(sphere_visible(v.ecef_to_clip, at(-1000, 0, 0), 10.0));     // behind
    EXPECT_FALSE(sphere_visible(v.ecef_to_clip, at(1000, 1000, 0), 10.0));   // 45 degrees right
    EXPECT_TRUE(sphere_visible(v.ecef_to_clip, at(1000, 1000, 0), 600.0));   // ...but reaching in
    EXPECT_FALSE(sphere_visible(v.ecef_to_clip, at(1000, 0, -1000), 10.0));  // well above
    EXPECT_FALSE(sphere_visible(v.ecef_to_clip, at(0.05, 0, 0), 0.01));      // nearer than near
}

TEST(TerrainLod, PicksTilesThatCoverTheViewOnceAndAreFineEnough) {
    const Tree t = make_tree();
    const LodView v = view_from(-30.0);
    const std::vector<std::uint32_t> picked = select(t, v);
    ASSERT_FALSE(picked.empty());
    // Never a tile and its ancestor.
    for (const std::uint32_t a : picked) {
        for (const std::uint32_t b : picked) {
            EXPECT_FALSE(is_ancestor(t.keys[a], t.keys[b]))
                << to_string(t.keys[a]) << " and " << to_string(t.keys[b]);
        }
    }
    // Every finest tile in view is covered by one picked tile.
    for (std::size_t i = 0; i < t.keys.size(); ++i) {
        const TileKey& k = t.keys[i];
        if (k.level != kFinest ||
            !sphere_visible(v.ecef_to_clip, t.tiles[i].centre_ecef - v.eye_ecef,
                            t.tiles[i].radius_m)) {
            continue;
        }
        const auto covers = std::count_if(picked.begin(), picked.end(), [&](std::uint32_t p) {
            return p == i || is_ancestor(t.keys[p], k);
        });
        EXPECT_EQ(covers, 1) << to_string(k);
    }
    // Each picked tile that could split shows at most pixel_error.
    for (const std::uint32_t p : picked) {
        const LodTile& tile = t.tiles[p];
        if (tile.children[0] == kNoTile) {
            continue;
        }
        const double d = glm::length(tile.centre_ecef - v.eye_ecef) - tile.radius_m;
        EXPECT_LE(screen_error_px(tile.error_m, d, v.pixels_per_tangent), v.pixel_error)
            << to_string(t.keys[p]);
    }
    // Fine near the eye, coarser far away.
    const auto [lo, hi] = std::minmax_element(picked.begin(), picked.end(), [&](auto a, auto b) {
        return t.keys[a].level < t.keys[b].level;
    });
    EXPECT_EQ(t.keys[*hi].level, kFinest);
    EXPECT_LT(t.keys[*lo].level, kFinest);
}

TEST(TerrainLod, LookingAwayPicksNothingAndALooserErrorPicksFewer) {
    const Tree t = make_tree();
    // Straight up: no tile in view.
    EXPECT_TRUE(select(t, view_from(89.0)).empty());
    LodView v = view_from(-30.0);
    const std::size_t tight = select(t, v).size();
    v.pixel_error = 8.0;
    EXPECT_LT(select(t, v).size(), tight);
    // Turned round, a different set.
    EXPECT_NE(select(t, view_from(-30.0, 180.0)), select(t, view_from(-30.0)));
}

TEST(TerrainLod, StopsAtTheScratchSpaceItIsGiven) {
    const Tree t = make_tree();
    std::vector<std::uint32_t> stack(t.tiles.size());
    std::vector<std::uint32_t> out(3);
    EXPECT_EQ(select_tiles(t.tiles, t.roots, view_from(-30.0), stack, out), 3U);
}

}  // namespace
}  // namespace fovea
