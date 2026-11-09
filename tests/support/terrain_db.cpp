#include "support/terrain_db.hpp"

#include <random>

#include <gtest/gtest.h>

#include "fovea/terrain/tile_file.hpp"

namespace fovea::test {

const std::filesystem::path& test_root() {
    struct Root {
        std::filesystem::path path;
        Root()
            : path(std::filesystem::temp_directory_path() /
                   ("fovea-terrain-tests-" + std::to_string(std::random_device{}()))) {}
        ~Root() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };
    static const Root root;
    return root.path;
}

TestDb make_db(const std::string& name, const GeoBounds& area,
               std::initializer_list<std::uint8_t> levels, std::uint32_t samples,
               const std::function<double(std::uint8_t, double, double)>& height) {
    TestDb db{test_root() / name, {}};
    std::filesystem::remove_all(db.dir);
    db.manifest.name = name;
    db.manifest.area = area;
    db.manifest.elevation_samples = samples;
    for (const std::uint8_t level : levels) {
        const TileKey sw = tile_at(area.south_deg, area.west_deg, level);
        const TileKey ne = tile_at(area.north_deg, area.east_deg, level);
        db.manifest.levels.push_back({level, sw.x, ne.x, sw.y, ne.y});
        for (std::uint32_t x = sw.x; x <= ne.x; ++x) {
            for (std::uint32_t y = sw.y; y <= ne.y; ++y) {
                const TileKey key{level, x, y};
                const GeoBounds b = tile_bounds(key);
                ElevationGrid g;
                g.size = samples;
                for (std::uint32_t r = 0; r < samples; ++r) {
                    for (std::uint32_t c = 0; c < samples; ++c) {
                        const double lat =
                            b.south_deg + (b.north_deg - b.south_deg) * r / (samples - 1);
                        const double lon =
                            b.west_deg + (b.east_deg - b.west_deg) * c / (samples - 1);
                        g.heights.push_back(static_cast<float>(height(level, lat, lon)));
                    }
                }
                TileFile t;
                t.key = key;
                t.layers.push_back(encode_elevation(g));
                EXPECT_TRUE(write_tile(db.dir / tile_path(key), t));
            }
        }
    }
    EXPECT_TRUE(write_manifest(db.dir, db.manifest));
    return db;
}

std::unique_ptr<TerrainDatabase> open_db(const TestDb& t) {
    std::unique_ptr<TerrainDatabase> db;
    const ManifestResult r = TerrainDatabase::open(t.dir, db);
    EXPECT_TRUE(r.errors.empty()) << (r.errors.empty() ? "" : r.errors[0]);
    return db;
}

}  // namespace fovea::test
