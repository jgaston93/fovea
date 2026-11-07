// fovea-compiler on synthetic sources (milestone 3 step 3): GeoTIFFs whose
// heights are planes in their own coordinates, so bilinear sampling is exact
// and every compiled height has a known answer. The real KHSV database is
// checked against its sources by tools/check-elevation.py.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <ranges>
#include <string>
#include <vector>

#include <gdal.h>
#include <gtest/gtest.h>
#include <ogr_srs_api.h>

#include "compile.hpp"
#include "elevation.hpp"
#include "fovea/terrain/database.hpp"
#include "fovea/terrain/tile_file.hpp"
#include "settings.hpp"

namespace fovea::compiler {
namespace {

namespace fs = std::filesystem;

// A per-run directory under the temp directory, removed at exit. Random,
// as ctest runs each test in its own process, in parallel.
const fs::path& test_root() {
    struct Root {
        fs::path path;
        Root()
            : path(fs::temp_directory_path() /
                   ("fovea-compiler-tests-" + std::to_string(std::random_device{}()))) {
            fs::create_directories(path);
        }
        ~Root() {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    };
    static const Root root;
    return root.path;
}

// A geographic NAD83 point to UTM 16N metres, and back.
struct Utm {
    OGRSpatialReferenceH geo = OSRNewSpatialReference(nullptr);
    OGRSpatialReferenceH utm = OSRNewSpatialReference(nullptr);
    OGRCoordinateTransformationH to_utm = nullptr;
    Utm() {
        OSRImportFromEPSG(geo, 4269);
        OSRImportFromEPSG(utm, 26916);
        OSRSetAxisMappingStrategy(geo, OAMS_TRADITIONAL_GIS_ORDER);
        OSRSetAxisMappingStrategy(utm, OAMS_TRADITIONAL_GIS_ORDER);
        to_utm = OCTNewCoordinateTransformation(geo, utm);
    }
    ~Utm() {
        OCTDestroyCoordinateTransformation(to_utm);
        OSRDestroySpatialReference(geo);
        OSRDestroySpatialReference(utm);
    }
    Utm(const Utm&) = delete;
    Utm& operator=(const Utm&) = delete;
    std::pair<double, double> operator()(double lat, double lon) const {
        double x = lon;
        double y = lat;
        OCTTransform(to_utm, 1, &x, &y, nullptr);
        return {x, y};
    }
};

// A one-band Float32 GeoTIFF: `value(x, y)` at each pixel centre, in the
// raster's own coordinates.
void write_raster(const fs::path& file, int epsg, double x0, double y0, double pixel, int width,
                  int height, double nodata, const std::function<double(double, double)>& value,
                  double scale = 1.0) {
    GDALDriverH gtiff = GDALGetDriverByName("GTiff");
    GDALDatasetH ds = GDALCreate(gtiff, file.c_str(), width, height, 1, GDT_Float32, nullptr);
    ASSERT_NE(ds, nullptr);
    double gt[6] = {x0, pixel, 0.0, y0, 0.0, -pixel};
    GDALSetGeoTransform(ds, gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, epsg);
    GDALSetSpatialRef(ds, srs);
    OSRDestroySpatialReference(srs);
    GDALRasterBandH band = GDALGetRasterBand(ds, 1);
    GDALSetRasterNoDataValue(band, nodata);
    GDALSetRasterScale(band, scale);
    std::vector<float> row(static_cast<std::size_t>(width));
    for (int r = 0; r < height; ++r) {
        for (int c = 0; c < width; ++c) {
            row[static_cast<std::size_t>(c)] =
                static_cast<float>(value(x0 + (c + 0.5) * pixel, y0 - (r + 0.5) * pixel));
        }
        ASSERT_EQ(
            GDALRasterIO(band, GF_Write, 0, r, width, 1, row.data(), width, 1, GDT_Float32, 0, 0),
            CE_None);
    }
    GDALClose(ds);
}

constexpr double kNoData = -999999.0;
constexpr double kOtherNoData = -3.4028230607370965e38;  // as some 3DEP projects use

// The scene: two root tiles at level 15 near KHSV, built to level 16.
//   old: a 2 m DEM over all but the eastmost 150 m (UTM), plane A
//   new: a 1 m DEM over the western part, A + 25 m, with a 100 m hole
//   backdrop: geographic, 0.0001 degrees, plane D in latitude and longitude
//   geoid: geographic in 0-360 longitudes, linear
struct Scene {
    static constexpr std::uint8_t kRoot = 15;
    Utm utm;
    fs::path dir;
    GeoBounds extent;  // the two root tiles
    double e_min = 0, e_max = 0, n_min = 0, n_max = 0;
    double e_new = 0;               // the new DEM covers e < e_new
    double e_old = 0;               // the old DEM covers e < e_old
    double hole_e = 0, hole_n = 0;  // the hole's south-west corner (100 m square)

    static double plane_a(double e, double n) {
        return 150.0 + 0.01 * (e - 500000.0) - 0.02 * (n - 3830000.0);
    }
    static double plane_d(double lat, double lon) {
        return 120.0 + 900.0 * (lat - 34.6) + 400.0 * (lon + 86.8);
    }
    static double geoid(double lat, double lon) {
        return -29.0 + 0.5 * (lat - 34.0) + 0.25 * (lon + 87.0);
    }

    // `rough`: bumps on both DEMs, so a filter or extrapolation that reads
    // different samples on either side of an edge shows (planes hide it).
    explicit Scene(const std::string& name, bool rough = false) : dir(test_root() / name) {
        fs::create_directories(dir / "cache" / "dem");
        fs::create_directories(dir / "cache" / "geoid");
        const TileKey k = tile_at(34.637, -86.775, kRoot);
        const GeoBounds a = tile_bounds(k);
        const GeoBounds b = tile_bounds({kRoot, k.x + 1, k.y});
        extent = {a.south_deg, a.north_deg, a.west_deg, b.east_deg};
        // The extent's corners in UTM, with a 60 m margin.
        e_min = n_min = 1e300;
        e_max = n_max = -1e300;
        for (const double lat : {extent.south_deg, extent.north_deg}) {
            for (const double lon : {extent.west_deg, extent.east_deg}) {
                const auto [e, n] = utm(lat, lon);
                e_min = std::min(e_min, std::floor(e) - 60);
                e_max = std::max(e_max, std::ceil(e) + 60);
                n_min = std::min(n_min, std::floor(n) - 60);
                n_max = std::max(n_max, std::ceil(n) + 60);
            }
        }
        e_old = e_max - 210;  // the eastmost ~150 m of the extent is the backdrop's
        e_new = std::floor((e_min + e_max) / 2);
        hole_e = e_new - 300;
        hole_n = std::floor((n_min + n_max) / 2);

        const auto w = [](double from, double to, double px) {
            return static_cast<int>(std::lround((to - from) / px));
        };
        const auto ground = [rough](double e, double n) {
            return plane_a(e, n) + (rough ? 3.0 * std::sin(e / 7.0) * std::cos(n / 11.0) : 0.0);
        };
        write_raster(dir / "cache/dem/old.tif", 26916, e_min, n_max, 2.0, w(e_min, e_old, 2.0),
                     w(n_min, n_max, 2.0), kNoData, ground);
        const double he = hole_e;
        const double hn = hole_n;
        write_raster(dir / "cache/dem/new.tif", 26916, e_min, n_max, 1.0, w(e_min, e_new, 1.0),
                     w(n_min, n_max, 1.0), kOtherNoData, [he, hn, ground](double e, double n) {
                         const bool hole = e > he && e < he + 100 && n > hn && n < hn + 100;
                         return hole ? kOtherNoData : ground(e, n) + 25.0;
                     });
        write_raster(dir / "cache/dem/backdrop.tif", 4269, -86.9, 34.7, 0.0001, 2000, 1500, kNoData,
                     [](double lon, double lat) { return plane_d(lat, lon); });
        write_raster(dir / "cache/geoid/geoid.tif", 4269, 272.5, 35.5, 0.01, 100, 100, kNoData,
                     [](double lon, double lat) { return geoid(lat, lon - 360.0); });
    }

    // The expected NAVD88 height at a point, and whether it is clear of
    // every source edge by `margin` metres (sampled heights blend there).
    std::pair<double, bool> expected(double lat, double lon, double margin) const {
        const auto [e, n] = utm(lat, lon);
        const auto near = [margin](double v, double edge) { return std::abs(v - edge) < margin; };
        const bool in_hole = e > hole_e && e < hole_e + 100 && n > hole_n && n < hole_n + 100;
        const bool clear = !near(e, e_new) && !near(e, e_old) && !near(e, hole_e) &&
                           !near(e, hole_e + 100) && !near(n, hole_n) && !near(n, hole_n + 100);
        if (e < e_new && !in_hole) {
            return {plane_a(e, n) + 25.0, clear};
        }
        if (e < e_old) {
            return {plane_a(e, n), clear};
        }
        return {plane_d(lat, lon), clear};
    }

    std::string settings(const std::string& extra_area = "") const {
        const GeoBounds a = extra_area.empty()
                                ? GeoBounds{extent.south_deg + 0.001, extent.north_deg - 0.001,
                                            extent.west_deg + 0.001, extent.east_deg - 0.001}
                                : GeoBounds{};
        std::string area = extra_area;
        if (area.empty()) {
            area = "area = { south = " + std::to_string(a.south_deg) +
                   ", north = " + std::to_string(a.north_deg) +
                   ", west = " + std::to_string(a.west_deg) +
                   ", east = " + std::to_string(a.east_deg) + " }\n";
        }
        return "name = \"synthetic\"\n" + area +
               "output = \"db\"\n"
               "[levels]\nroot = 15\nfinest = 16\n"
               "[elevation]\nsamples = 33\ncache = \"cache\"\n"
               "sources = [\"dem/new*.tif\", \"dem/old.tif\", \"dem/backdrop.tif\"]\n"
               "geoid = \"geoid/geoid.tif\"\n"
               "[[credit]]\nlayer = \"elevation\"\nname = \"synthetic\"\ncredit = \"none\"\n";
    }

    CompileResult build(const std::string& text) const {
        const auto loaded = parse_settings(text, "settings.toml", dir);
        EXPECT_TRUE(loaded.settings.has_value()) << (loaded.errors.empty() ? "" : loaded.errors[0]);
        if (!loaded.settings) {
            return {};
        }
        return compile(*loaded.settings, 4, nullptr);
    }
};

ElevationGrid read_grid(const fs::path& db, const TileKey& key) {
    const TileResult r = read_tile(db / tile_path(key), key);
    EXPECT_TRUE(r.tile.has_value()) << to_string(key) << ": " << r.detail;
    if (!r.tile) {
        return {};
    }
    const auto g = decode_elevation(*r.tile->find(kElevationLayer));
    EXPECT_TRUE(g.has_value());
    return g.value_or(ElevationGrid{});
}

class Compiler : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        GDALAllRegister();
        scene_ = std::make_unique<Scene>("shared");
        result_ = std::make_unique<CompileResult>(scene_->build(scene_->settings()));
    }
    static void TearDownTestSuite() {
        result_.reset();
        scene_.reset();
    }
    static fs::path db() { return scene_->dir / "db"; }
    static std::unique_ptr<Scene> scene_;
    static std::unique_ptr<CompileResult> result_;
};
std::unique_ptr<Scene> Compiler::scene_;
std::unique_ptr<CompileResult> Compiler::result_;

TEST_F(Compiler, BuildsEveryDescendantOfTheRoots) {
    ASSERT_TRUE(result_->ok) << (result_->errors.empty() ? "" : result_->errors[0]);
    const DatabaseManifest& m = result_->manifest;
    ASSERT_EQ(m.levels.size(), 2U);
    EXPECT_EQ(m.levels[0].level, 15);
    EXPECT_EQ(m.levels[0].x1 - m.levels[0].x0, 1U);  // two roots, side by side
    EXPECT_EQ(m.levels[0].y1, m.levels[0].y0);
    EXPECT_EQ(m.levels[1].level, 16);
    EXPECT_EQ(m.levels[1].x0, m.levels[0].x0 * 2);
    EXPECT_EQ(m.levels[1].x1, m.levels[0].x1 * 2 + 1);
    EXPECT_EQ(m.levels[1].y1 - m.levels[1].y0, 1U);
    // The written manifest is the same, and nothing is left half-built.
    std::unique_ptr<TerrainDatabase> opened;
    const auto r = TerrainDatabase::open(db(), opened);
    ASSERT_TRUE(r.manifest.has_value()) << (r.errors.empty() ? "" : r.errors[0]);
    EXPECT_EQ(manifest_text(*r.manifest), manifest_text(m));
    EXPECT_EQ(r.manifest->elevation_samples, 33U);
    ASSERT_EQ(r.manifest->sources.size(), 1U);
    EXPECT_FALSE(fs::exists(fs::path(db()).concat(".part")));
}

// At the finest level each sample is the highest-priority source, bilinear,
// plus the geoid: exact for planes, wherever no source edge is near.
TEST_F(Compiler, FinestSamplesAreTheSourcesPlusTheGeoid) {
    ASSERT_TRUE(result_->ok);
    const LevelTiles& fine = result_->manifest.levels[1];
    int checked = 0;
    int from_new = 0;
    int from_old = 0;
    int from_backdrop = 0;
    for (std::uint32_t x = fine.x0; x <= fine.x1; ++x) {
        for (std::uint32_t y = fine.y0; y <= fine.y1; ++y) {
            const TileKey key{16, x, y};
            const ElevationGrid g = read_grid(db(), key);
            const GeoBounds b = tile_bounds(key);
            const double step = (b.east_deg - b.west_deg) / (g.size - 1);
            for (std::uint32_t r = 0; r < g.size; ++r) {
                for (std::uint32_t c = 0; c < g.size; ++c) {
                    const double lat = b.south_deg + r * step;
                    const double lon = b.west_deg + c * step;
                    const auto [h, clear] = scene_->expected(lat, lon, 3.0);
                    if (!clear) {
                        continue;
                    }
                    ASSERT_NEAR(g.at(r, c), h + Scene::geoid(lat, lon), 1e-3)
                        << to_string(key) << " row " << r << " column " << c;
                    ++checked;
                    const auto [e, n] = scene_->utm(lat, lon);
                    const bool hole = e > scene_->hole_e && e < scene_->hole_e + 100 &&
                                      n > scene_->hole_n && n < scene_->hole_n + 100;
                    if (e < scene_->e_new && !hole) {
                        ++from_new;
                    } else if (e < scene_->e_old) {
                        ++from_old;
                    } else {
                        ++from_backdrop;
                    }
                }
            }
        }
    }
    EXPECT_GT(checked, 7000);
    // Each source (and the hole in the newest) is exercised.
    EXPECT_GT(from_new, 1000);
    EXPECT_GT(from_old, 1000);
    EXPECT_GT(from_backdrop, 100);
}

// Above the finest level samples are cell averages; for a plane, the value
// at the cell's centre.
TEST_F(Compiler, CoarseSamplesAreCellAverages) {
    ASSERT_TRUE(result_->ok);
    const LevelTiles& roots = result_->manifest.levels[0];
    int checked = 0;
    for (std::uint32_t x = roots.x0; x <= roots.x1; ++x) {
        const TileKey key{15, x, roots.y0};
        const ElevationGrid g = read_grid(db(), key);
        const GeoBounds b = tile_bounds(key);
        const double step = (b.east_deg - b.west_deg) / (g.size - 1);
        for (std::uint32_t r = 0; r < g.size; ++r) {
            for (std::uint32_t c = 0; c < g.size; ++c) {
                const double lat = b.south_deg + r * step;
                const double lon = b.west_deg + c * step;
                const auto [h, clear] = scene_->expected(lat, lon, 30.0);  // a cell is ~17 m
                if (clear) {
                    ASSERT_NEAR(g.at(r, c), h + Scene::geoid(lat, lon), 0.02)
                        << to_string(key) << " row " << r << " column " << c;
                    ++checked;
                }
            }
        }
    }
    EXPECT_GT(checked, 600);
}

void expect_shared_edges(const fs::path& db, const DatabaseManifest& m) {
    // Finest first: coarser levels are filtered from it.
    for (const LevelTiles& l : std::views::reverse(m.levels)) {
        for (std::uint32_t x = l.x0; x <= l.x1; ++x) {
            for (std::uint32_t y = l.y0; y <= l.y1; ++y) {
                const ElevationGrid g = read_grid(db, {l.level, x, y});
                const std::uint32_t last = g.size - 1;
                if (x < l.x1) {
                    const ElevationGrid east = read_grid(db, {l.level, x + 1, y});
                    for (std::uint32_t i = 0; i < g.size; ++i) {
                        ASSERT_NEAR(g.at(i, last), east.at(i, 0), 1e-4)
                            << int{l.level} << "/" << x << "/" << y << " row " << i;
                    }
                }
                if (y < l.y1) {
                    const ElevationGrid north = read_grid(db, {l.level, x, y + 1});
                    for (std::uint32_t i = 0; i < g.size; ++i) {
                        ASSERT_NEAR(g.at(last, i), north.at(0, i), 1e-4)
                            << int{l.level} << "/" << x << "/" << y << " column " << i;
                    }
                }
            }
        }
    }
}

TEST_F(Compiler, NeighboursShareTheirEdgeSamples) {
    ASSERT_TRUE(result_->ok);
    expect_shared_edges(db(), result_->manifest);
}

// Two root tiles side by side, then a level more, on rough ground: the
// tiles either side of an edge filter (and, at the database's edge,
// extrapolate) the same child samples.
TEST(CompilerRough, NeighboursShareTheirEdgeSamples) {
    GDALAllRegister();
    const Scene scene("rough", true);
    std::string text = scene.settings();
    text.replace(text.find("root = 15"), 9, "root = 14");
    text.replace(text.find("samples = 33"), 12, "samples = 17");
    const auto r = scene.build(text);
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    ASSERT_EQ(r.manifest.levels.size(), 3U);
    expect_shared_edges(scene.dir / "db", r.manifest);
}

// A tile's error bounds how far it is from its children anywhere: the
// 25 m step at the newer DEM's edge shows in the roots, not the finest.
TEST_F(Compiler, ErrorsBoundTheDistanceToTheChildren) {
    ASSERT_TRUE(result_->ok);
    const LevelTiles& roots = result_->manifest.levels[0];
    for (std::uint32_t x = roots.x0; x <= roots.x1; ++x) {
        const TileKey key{15, x, roots.y0};
        const ElevationGrid g = read_grid(db(), key);
        const GeoBounds b = tile_bounds(key);
        double largest = 0.0;
        for (const TileKey& ck : children(key)) {
            const ElevationGrid child = read_grid(db(), ck);
            EXPECT_EQ(child.error_m, 0.0F);
            const GeoBounds cb = tile_bounds(ck);
            // Between the samples too: a quarter step apart.
            const double step = (cb.east_deg - cb.west_deg) / ((child.size - 1) * 4);
            for (int r = 0; r <= static_cast<int>(child.size - 1) * 4; ++r) {
                for (int c = 0; c <= static_cast<int>(child.size - 1) * 4; ++c) {
                    const double lat = cb.south_deg + r * step;
                    const double lon = cb.west_deg + c * step;
                    largest = std::max(
                        largest, std::abs(child.sample(cb, lat, lon) - g.sample(b, lat, lon)));
                }
            }
        }
        EXPECT_LE(largest, static_cast<double>(g.error_m) + 1e-3) << to_string(key);
        EXPECT_GE(static_cast<double>(g.error_m), largest - 1e-3) << to_string(key);
        if (x == roots.x0) {
            EXPECT_GT(g.error_m, 5.0F) << "the 25 m step lies in the western root";
        }
    }
}

TEST_F(Compiler, TheReaderAnswersHeightsFromTheCompiledDatabase) {
    ASSERT_TRUE(result_->ok);
    std::unique_ptr<TerrainDatabase> opened;
    ASSERT_TRUE(TerrainDatabase::open(db(), opened).manifest.has_value());
    const DatabaseTerrain terrain(*opened);
    const double lat = (scene_->extent.south_deg + scene_->extent.north_deg) / 2 + 0.0011;
    const double lon = scene_->extent.west_deg + 0.0013;
    const auto [h, clear] = scene_->expected(lat, lon, 5.0);
    ASSERT_TRUE(clear);
    const auto got = terrain.height_at(lat, lon);
    ASSERT_TRUE(got.has_value());
    // Bilinear between ~1 m samples of a plane in UTM: centimetres at most.
    EXPECT_NEAR(*got, h + Scene::geoid(lat, lon), 0.01);
    EXPECT_TRUE(terrain.problems().empty());
}

TEST(CompilerFailures, AreaBeyondTheSourcesFailsAndWritesNothing) {
    GDALAllRegister();
    const Scene scene("beyond");
    // West of every source (the geoid still covers it).
    const std::string area =
        "area = { south = 34.6, north = 34.601, west = -87.3, east = -87.299 }\n";
    const auto r = scene.build(scene.settings(area));
    ASSERT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].find("no elevation source"), std::string::npos) << r.errors[0];
    EXPECT_FALSE(fs::exists(scene.dir / "db"));
    EXPECT_FALSE(fs::exists(scene.dir / "db.part"));
}

TEST(CompilerFailures, DoesNotReplaceADirectoryThatIsNotADatabase) {
    GDALAllRegister();
    const Scene scene("not_a_db");
    fs::create_directories(scene.dir / "db");
    std::ofstream(scene.dir / "db" / "keep.txt") << "mine";
    const auto r = scene.build(scene.settings());
    ASSERT_FALSE(r.ok);
    EXPECT_NE(r.errors[0].find("isn't a database"), std::string::npos) << r.errors[0];
    EXPECT_TRUE(fs::exists(scene.dir / "db" / "keep.txt"));
}

TEST(CompilerFailures, ReplacesAnOlderDatabase) {
    GDALAllRegister();
    const Scene scene("rebuild");
    ASSERT_TRUE(scene.build(scene.settings()).ok);
    std::ofstream(scene.dir / "db" / "stale.txt") << "from the first build";
    ASSERT_TRUE(scene.build(scene.settings()).ok);
    EXPECT_FALSE(fs::exists(scene.dir / "db" / "stale.txt"));
    EXPECT_TRUE(fs::exists(scene.dir / "db" / "manifest.toml"));
}

TEST(CompilerFailures, AnOutputEndingInASlashBuildsBesideIt) {
    GDALAllRegister();
    const Scene scene("slash");
    std::string text = scene.settings();
    text.replace(text.find("output = \"db\""), 13, "output = \"db/\"");
    for (int build = 0; build < 2; ++build) {  // the second replaces the first
        const auto r = scene.build(text);
        ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
        EXPECT_TRUE(fs::exists(scene.dir / "db" / "manifest.toml"));
        EXPECT_FALSE(fs::exists(scene.dir / "db" / ".part"));
        EXPECT_FALSE(fs::exists(scene.dir / "db.part"));
        EXPECT_FALSE(fs::exists(scene.dir / "db.old"));
    }
}

TEST(CompilerFailures, AnInterruptedReplacementIsLeftAlone) {
    GDALAllRegister();
    const Scene scene("interrupted");
    fs::create_directories(scene.dir / "db.old");
    std::ofstream(scene.dir / "db.old" / "manifest.toml") << "the older database";
    const auto r = scene.build(scene.settings());
    ASSERT_FALSE(r.ok);
    EXPECT_NE(r.errors[0].find("interrupted"), std::string::npos) << r.errors[0];
    EXPECT_TRUE(fs::exists(scene.dir / "db.old" / "manifest.toml"));
}

// --- Sources and the geoid ---

TEST(CompilerSources, RejectsHeightsNotStoredInMetres) {
    GDALAllRegister();
    const fs::path dir = test_root() / "scaled";
    fs::create_directories(dir);
    write_raster(
        dir / "scaled.tif", 4269, -86.8, 34.7, 0.001, 10, 10, kNoData,
        [](double, double) { return 100.0; }, 0.01);
    std::string error;
    EXPECT_EQ(ElevationSources::open({dir / "scaled.tif"}, error), nullptr);
    EXPECT_NE(error.find("metres"), std::string::npos) << error;
}

// A sample exactly on a source's last pixel centre (here every grid point
// is a pixel centre, so the tile's north and east edges are the last ones)
// comes from that source.
TEST(CompilerSources, SamplesOnTheLastPixelCentre) {
    GDALAllRegister();
    const fs::path dir = test_root() / "last_pixel";
    fs::create_directories(dir);
    constexpr std::uint32_t kSamples = 9;
    const TileKey key = tile_at(34.637, -86.775, 14);
    const GeoBounds b = tile_bounds(key);
    const double px = (b.east_deg - b.west_deg) / (kSamples - 1);
    write_raster(dir / "exact.tif", 4269, b.west_deg - px / 2, b.north_deg + px / 2, px, kSamples,
                 kSamples, kNoData,
                 [](double lon, double lat) { return 100.0 + 1000.0 * (lat - 34.6 + lon + 86.8); });
    std::string error;
    const auto sources = ElevationSources::open({dir / "exact.tif"}, error);
    ASSERT_NE(sources, nullptr) << error;
    std::vector<float> h;
    ASSERT_TRUE(sources->heights(key, kSamples, h, error)) << error;
    const double lat = b.north_deg;
    const double lon = b.east_deg;
    EXPECT_NEAR(h.back(), 100.0 + 1000.0 * (lat - 34.6 + lon + 86.8), 1e-3);
    EXPECT_NEAR(h.front(), 100.0 + 1000.0 * (b.south_deg - 34.6 + b.west_deg + 86.8), 1e-3);
}

TEST(CompilerSources, TheGeoidMustBeGeographicAndAreasMayNotCrossItsEdge) {
    GDALAllRegister();
    const fs::path dir = test_root() / "geoid";
    fs::create_directories(dir);
    write_raster(dir / "projected.tif", 26916, 500000, 3900000, 1000, 10, 10, kNoData,
                 [](double, double) { return -29.0; });
    std::string error;
    EXPECT_FALSE(Geoid::load(dir / "projected.tif", {34.0, 35.0, -87.0, -86.0}, error));
    EXPECT_NE(error.find("geographic"), std::string::npos) << error;

    // 0-360 longitudes: an area west of 180 W is read at 180 E and beyond,
    // and one across 0 E doesn't fit the grid.
    write_raster(dir / "global.tif", 4269, 0.0, 60.0, 1.0, 360, 120, kNoData,
                 [](double lon, double) { return lon; });
    error.clear();
    const auto g = Geoid::load(dir / "global.tif", {34.0, 35.0, -87.0, -86.0}, error);
    ASSERT_TRUE(g.has_value()) << error;
    EXPECT_NEAR(g->at(34.5, -86.5), 273.5, 1e-6);
    EXPECT_FALSE(Geoid::load(dir / "global.tif", {34.0, 35.0, -1.0, 1.0}, error));
    EXPECT_NE(error.find("crosses"), std::string::npos) << error;
}

// --- Settings ---

TEST(CompilerSettings, WildcardsMatchRunsOfCharacters) {
    EXPECT_TRUE(wildcard_match("*.tif", "a.tif"));
    EXPECT_TRUE(wildcard_match("USGS_*_B24.tif", "USGS_1M_16_x51y383_AL_19County_B24.tif"));
    EXPECT_FALSE(wildcard_match("USGS_*_B24.tif", "USGS_1M_16_x51y383_AL_11County_B23.tif"));
    EXPECT_TRUE(wildcard_match("a*b*c", "aXXbYYc"));
    EXPECT_TRUE(wildcard_match("a*", "a"));
    EXPECT_FALSE(wildcard_match("a*c", "ab"));
    EXPECT_TRUE(wildcard_match("*", ""));
    EXPECT_FALSE(wildcard_match("", "a"));
    EXPECT_TRUE(wildcard_match("a**b", "ab"));
}

TEST(CompilerSettings, ListsFilesNewestFirstAndFlagsTheRest) {
    const fs::path dir = test_root() / "settings";
    fs::create_directories(dir / "cache" / "dem");
    for (const char* f : {"b_2024.tif", "a_2024.tif", "c_2019.tif", "unlisted_2001.tif"}) {
        std::ofstream(dir / "cache" / "dem" / f) << "x";
    }
    fs::create_directories(dir / "cache" / "geoid");
    const std::string text =
        "name = \"t\"\narea = { south = 1.0, north = 2.0, west = 3.0, east = 4.0 }\n"
        "output = \"db\"\n[levels]\nroot = 3\nfinest = 5\n"
        "[elevation]\nsamples = 65\ncache = \"cache\"\n"
        "sources = [\"dem/*_2024.tif\", \"dem/*.tif\"]\ngeoid = \"geoid/g.tif\"\n";
    const auto r = parse_settings(text, "s.toml", dir);
    ASSERT_TRUE(r.settings.has_value()) << (r.errors.empty() ? "" : r.errors[0]);
    const auto& files = r.settings->elevation;
    ASSERT_EQ(files.size(), 4U);  // the second pattern takes the rest, once each
    EXPECT_EQ(files[0].filename(), "a_2024.tif");
    EXPECT_EQ(files[1].filename(), "b_2024.tif");
    EXPECT_EQ(files[2].filename(), "c_2019.tif");
    EXPECT_EQ(files[3].filename(), "unlisted_2001.tif");
    EXPECT_TRUE(r.warnings.empty());
    EXPECT_EQ(r.settings->samples, 65U);

    // Another spelling of the same directory names the same files, once.
    std::string spelled = text;
    spelled.replace(spelled.find("\"dem/*.tif\""), 11, "\"./dem/../dem/*.tif\"");
    const auto sp = parse_settings(spelled, "s.toml", dir);
    ASSERT_TRUE(sp.settings.has_value()) << (sp.errors.empty() ? "" : sp.errors[0]);
    EXPECT_EQ(sp.settings->elevation.size(), 4U);
    EXPECT_TRUE(sp.warnings.empty());
    EXPECT_EQ(r.settings->output, dir / "db");

    // Only the 2024 pattern: the others are flagged as left out.
    std::string narrow = text;
    narrow.replace(narrow.find(", \"dem/*.tif\""), 13, "");
    const auto n = parse_settings(narrow, "s.toml", dir);
    ASSERT_TRUE(n.settings.has_value());
    EXPECT_EQ(n.settings->elevation.size(), 2U);
    ASSERT_EQ(n.warnings.size(), 2U);
    EXPECT_NE(n.warnings[0].find("c_2019.tif"), std::string::npos);
}

TEST(CompilerSettings, RejectsMistakes) {
    const fs::path dir = test_root() / "settings_bad";
    fs::create_directories(dir / "cache" / "dem");
    std::ofstream(dir / "cache" / "dem" / "a.tif") << "x";
    const auto errors = [&](const std::string& text) {
        return parse_settings(text, "s.toml", dir).errors;
    };
    const std::string good =
        "name = \"t\"\narea = { south = 1.0, north = 2.0, west = 3.0, east = 4.0 }\n"
        "output = \"db\"\n[levels]\nroot = 3\nfinest = 5\n"
        "[elevation]\nsamples = 65\ncache = \"cache\"\n"
        "sources = [\"dem/a.tif\"]\ngeoid = \"geoid/g.tif\"\n";
    EXPECT_TRUE(errors(good).empty());
    const auto with = [&](const std::string& from, const std::string& to) {
        std::string t = good;
        t.replace(t.find(from), from.size(), to);
        return errors(t);
    };
    EXPECT_FALSE(with("dem/a.tif", "dem/none*.tif").empty());  // matches nothing
    EXPECT_FALSE(with("finest = 5", "finest = 2").empty());    // root below finest
    EXPECT_FALSE(with("finest = 5", "finest = 31").empty());   // past the deepest level
    EXPECT_FALSE(with("root = 3", "root = -1").empty());
    EXPECT_FALSE(with("finest = 5", "finest = 12").empty());             // more than 8 levels
    EXPECT_FALSE(with("samples = 65", "samples = 4294967363").empty());  // wraps if narrowed
    EXPECT_FALSE(with("samples = 65", "samples = 64").empty());          // even
    EXPECT_FALSE(with("samples = 65", "samples = 1027").empty());        // past 1025
    EXPECT_FALSE(with("south = 1.0", "south = 2.5").empty());            // south above north
    EXPECT_FALSE(with("area = {", "area = \"missing.toml\"\nx = {").empty());
    EXPECT_FALSE(with("name = \"t\"\n", "").empty());
}

}  // namespace
}  // namespace fovea::compiler
