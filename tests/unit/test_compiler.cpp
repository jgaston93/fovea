// fovea-compiler on synthetic sources (milestone 3 steps 3 and 4): GeoTIFFs
// whose heights are planes in their own coordinates, so bilinear sampling is
// exact and every compiled height has a known answer, and imagery of solid
// colours and linear gradients, likewise. The real KHSV database is checked
// against its sources by tools/check-elevation.py and check-imagery.py.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <random>
#include <ranges>
#include <span>
#include <string>
#include <vector>

#include <gdal.h>
#include <gtest/gtest.h>
#include <ogr_srs_api.h>

#include "compile.hpp"
#include "datum.hpp"
#include "elevation.hpp"
#include "fovea/core/geodesy.hpp"
#include "fovea/terrain/database.hpp"
#include "fovea/terrain/tile_file.hpp"
#include "lights.hpp"
#include "runways.hpp"
#include "settings.hpp"
#include "support/glb.hpp"
#include "texture.hpp"

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
    // As the compiler takes the database's WGS-84 points to the sources'
    // NAD83(2011) (D-037; the settings' epoch).
    DatumShift datum = DatumShift::none();

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
        std::string why;
        datum = DatumShift::make(extent, 2026.0, why).value_or(DatumShift::none());
        EXPECT_TRUE(why.empty()) << why;
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

    // The expected WGS-84 ellipsoid height at a WGS-84 point: the sources'
    // at its NAD83(2011) point, plus the geoid there, moved to WGS-84's
    // ellipsoid; and whether it is clear of every source edge.
    std::pair<double, bool> ellipsoidal(double lat, double lon, double margin) const {
        const Nad83Point p = datum.to_nad83(lat, lon);
        const auto [h, clear] = expected(p.lat_deg, p.lon_deg, margin);
        return {h + geoid(p.lat_deg, p.lon_deg) + p.height_shift_m, clear};
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
               "[datum]\nepoch = 2026.0\n[elevation]\nsamples = 33\ncache = \"cache\"\n"
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
                    const auto [h, clear] = scene_->ellipsoidal(lat, lon, 3.0);
                    if (!clear) {
                        continue;
                    }
                    ASSERT_NEAR(g.at(r, c), h, 1e-3)
                        << to_string(key) << " row " << r << " column " << c;
                    ++checked;
                    const Nad83Point p = scene_->datum.to_nad83(lat, lon);
                    const auto [e, n] = scene_->utm(p.lat_deg, p.lon_deg);
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
                const auto [h, clear] = scene_->ellipsoidal(lat, lon, 30.0);  // a cell is ~17 m
                if (clear) {
                    ASSERT_NEAR(g.at(r, c), h, 0.02)
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
    const auto [h, clear] = scene_->ellipsoidal(lat, lon, 5.0);
    ASSERT_TRUE(clear);
    const auto got = terrain.height_at(lat, lon);
    ASSERT_TRUE(got.has_value());
    // Bilinear between ~1 m samples of a plane in UTM: centimetres at most.
    EXPECT_NEAR(*got, h, 0.01);
    EXPECT_TRUE(terrain.problems().empty());
}

// The database's frame (D-037): WGS 84 (G2139) at the settings' epoch,
// from the sources' NAD83(2011). Against PROJ 9.4's own cs2cs (EPSG:9754 to
// EPSG:6319, NAD83(2011) to ITRF2014 by the NGS's time-dependent Helmert)
// to a millimetre, so a ballpark transformation (no change), a lost epoch
// or a turned sign shows; NOAA's NCAT doesn't do ITRF.
TEST(CompilerDatum, MatchesProjAndDependsOnTheEpoch) {
    GDALAllRegister();
    const GeoBounds area{34.55, 34.73, -86.89, -86.66};
    std::string why;
    const auto d2026 = DatumShift::make(area, 2026.0, why);
    ASSERT_TRUE(d2026.has_value()) << why;
    const auto d2010 = DatumShift::make(area, 2010.0, why);
    ASSERT_TRUE(d2010.has_value()) << why;
    const double m_lat = 1.0 / 110900.0;  // degrees a metre, about, here
    const double m_lon = m_lat / std::cos(glm::radians(34.64));
    struct Case {
        const DatumShift& d;
        double lat, lon;          // WGS-84
        double nad_lat, nad_lon;  // cs2cs
        double nad_h;             // of a WGS-84 height of 0
    };
    for (const Case& c : {Case{*d2026, 34.6372, -86.7751, 34.637193250, -86.775090216, 1.300107730},
                          Case{*d2010, 34.6372, -86.7751, 34.637193367, -86.775092894, 1.312035566},
                          Case{*d2026, 34.70, -86.60, 34.699993211, -86.599990253, 1.300641499}}) {
        const Nad83Point p = c.d.to_nad83(c.lat, c.lon);
        EXPECT_NEAR((p.lat_deg - c.nad_lat) / m_lat, 0.0, 1e-3) << c.lat << ", " << c.lon;
        EXPECT_NEAR((p.lon_deg - c.nad_lon) / m_lon, 0.0, 1e-3) << c.lat << ", " << c.lon;
        EXPECT_NEAR(p.height_shift_m, -c.nad_h, 1e-3) << c.lat << ", " << c.lon;
    }
    // About a metre and a quarter apart, and moving: a quarter of a metre
    // from 2010 to 2026.
    const Nad83Point now = d2026->to_nad83(34.6372, -86.7751);
    const Nad83Point then = d2010->to_nad83(34.6372, -86.7751);
    const double across =
        std::hypot((now.lat_deg - 34.6372) / m_lat, (now.lon_deg + 86.7751) / m_lon);
    EXPECT_GT(across, 1.0);
    EXPECT_LT(across, 1.5);
    EXPECT_GT(
        std::hypot((now.lat_deg - then.lat_deg) / m_lat, (now.lon_deg - then.lon_deg) / m_lon),
        0.2);
    // Back again, to a tenth of a millimetre.
    double lat = now.lat_deg;
    double lon = now.lon_deg;
    d2026->to_wgs84(lat, lon);
    EXPECT_NEAR((lat - 34.6372) / m_lat, 0.0, 1e-4);
    EXPECT_NEAR((lon + 86.7751) / m_lon, 0.0, 1e-4);
    // Epochs that aren't years, refused.
    EXPECT_FALSE(DatumShift::make(area, 26.0, why).has_value());
    EXPECT_FALSE(DatumShift::make(area, std::nan(""), why).has_value());
}

// Settings must name the epoch, and the manifest records the frame.
TEST_F(Compiler, TheManifestRecordsTheFrame) {
    ASSERT_TRUE(result_->ok);
    ASSERT_TRUE(result_->manifest.frame.has_value());
    EXPECT_EQ(result_->manifest.frame->name, "WGS 84 (G2139)");
    EXPECT_EQ(result_->manifest.frame->epoch, 2026.0);
    std::unique_ptr<TerrainDatabase> opened;
    const auto r = TerrainDatabase::open(db(), opened);
    ASSERT_TRUE(r.manifest.has_value() && r.manifest->frame.has_value());
    EXPECT_EQ(r.manifest->frame->epoch, 2026.0);
    // Another frame is refused, not read as WGS-84.
    std::string other = manifest_text(*r.manifest);
    const auto name = other.find("WGS 84 (G2139)");
    ASSERT_NE(name, std::string::npos);
    other.replace(name, std::string("WGS 84 (G2139)").size(), "NAD83(2011)");
    const ManifestResult refused = parse_manifest(other, "manifest.toml");
    EXPECT_FALSE(refused.manifest.has_value());
    ASSERT_FALSE(refused.errors.empty());
    EXPECT_NE(refused.errors[0].find("[frame]"), std::string::npos);
    std::string text = scene_->settings();
    const auto at = text.find("[datum]\nepoch = 2026.0\n");
    ASSERT_NE(at, std::string::npos);
    text.erase(at, std::string("[datum]\nepoch = 2026.0\n").size());
    const auto loaded = parse_settings(text, "settings.toml", scene_->dir);
    ASSERT_FALSE(loaded.errors.empty());
    EXPECT_NE(loaded.errors[0].find("[datum]"), std::string::npos);
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

// --- Imagery (step 4) ---

// A three-band 8-bit RGB GeoTIFF in UTM 16N: `rgb(column, row)` per pixel.
void write_rgb(const fs::path& file, double x0, double y0, double pixel, int width, int height,
               const std::function<std::array<std::uint8_t, 3>(int, int)>& rgb) {
    GDALDriverH gtiff = GDALGetDriverByName("GTiff");
    GDALDatasetH ds = GDALCreate(gtiff, file.c_str(), width, height, 3, GDT_Byte, nullptr);
    ASSERT_NE(ds, nullptr);
    double gt[6] = {x0, pixel, 0.0, y0, 0.0, -pixel};
    GDALSetGeoTransform(ds, gt);
    OGRSpatialReferenceH srs = OSRNewSpatialReference(nullptr);
    OSRImportFromEPSG(srs, 26916);
    GDALSetSpatialRef(ds, srs);
    OSRDestroySpatialReference(srs);
    const std::array<GDALColorInterp, 3> interp = {GCI_RedBand, GCI_GreenBand, GCI_BlueBand};
    for (int b = 0; b < 3; ++b) {
        GDALRasterBandH band = GDALGetRasterBand(ds, b + 1);
        GDALSetRasterColorInterpretation(band, interp[static_cast<std::size_t>(b)]);
        std::vector<std::uint8_t> row(static_cast<std::size_t>(width));
        for (int r = 0; r < height; ++r) {
            for (int c = 0; c < width; ++c) {
                row[static_cast<std::size_t>(c)] = rgb(c, r)[static_cast<std::size_t>(b)];
            }
            ASSERT_EQ(
                GDALRasterIO(band, GF_Write, 0, r, width, 1, row.data(), width, 1, GDT_Byte, 0, 0),
                CE_None);
        }
    }
    GDALClose(ds);
}

// Imagery over the Scene, 4 m pixels (a level-16 tile of 64 texels has
// them about 4.8 x 3.9 m apart):
//   old (blue): everything; the lowest priority
//   new_a (red): the west, to 100 m past the middle
//   new_b (green): from 100 m before the middle to 300 m past it; new_a and
//     new_b are one pattern, so the middle of their overlap divides them
//     (all share their north and south edges, so that holds there too)
//   patch: a 400 m square in the west, the highest priority, red = 50 +
//     column and green = 50 + row (rows from the north): bilinear is exact
struct ImageryScene : Scene {
    static constexpr double kPixel = 4.0;
    static constexpr std::array<std::uint8_t, 3> kRed = {200, 0, 0};
    static constexpr std::array<std::uint8_t, 3> kGreen = {0, 200, 0};
    static constexpr std::array<std::uint8_t, 3> kBlue = {0, 0, 200};
    double e_mid = 0;
    double patch_e = 0;
    double patch_n = 0;  // its north-west corner

    explicit ImageryScene(const std::string& name) : Scene(name) {
        fs::create_directories(dir / "cache" / "img");
        e_mid = std::floor((e_min + e_max) / 2 / kPixel) * kPixel;
        patch_e = std::floor((e_min + 200) / kPixel) * kPixel;
        patch_n = std::floor((n_min + n_max) / 2 / kPixel) * kPixel;
        const auto px = [](double from, double to) {
            return static_cast<int>(std::lround((to - from) / kPixel));
        };
        const auto solid = [](std::array<std::uint8_t, 3> c) {
            return [c](int, int) { return c; };
        };
        const int rows = px(n_min, n_max);
        write_rgb(dir / "cache/img/old.tif", e_min, n_max, kPixel, px(e_min, e_max), rows,
                  solid(kBlue));
        write_rgb(dir / "cache/img/new_a.tif", e_min, n_max, kPixel, px(e_min, e_mid + 100), rows,
                  solid(kRed));
        write_rgb(dir / "cache/img/new_b.tif", e_mid - 100, n_max, kPixel,
                  px(e_mid - 100, e_mid + 300), rows, solid(kGreen));
        write_rgb(dir / "cache/img/patch.tif", patch_e, patch_n, kPixel, 100, 100,
                  [](int c, int r) {
                      return std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(50 + c),
                                                         static_cast<std::uint8_t>(50 + r), 128};
                  });
    }

    std::string imagery_settings(
        const std::string& format = "rgba8", int texels = 64,
        const std::string& sources = R"("img/patch.tif", "img/new_*.tif", "img/old.tif")") const {
        return settings() + "[imagery]\nformat = \"" + format +
               "\"\ntexels = " + std::to_string(texels) + "\ncache = \"cache\"\nsources = [" +
               sources + "]\n";
    }

    // The colour at a point (as doubles: the patch's gradient is exact
    // between whole values), and whether it is `margin` metres clear of
    // every source's edge.
    std::pair<std::array<double, 3>, bool> expected(double lat, double lon, double margin) const {
        const auto [e, n] = utm(lat, lon);
        const auto near = [margin](double v, double edge) { return std::abs(v - edge) < margin; };
        const double pe = patch_e + 100 * kPixel;
        const double ps = patch_n - 100 * kPixel;
        const bool in_patch = e > patch_e && e < pe && n > ps && n < patch_n;
        bool clear = !near(e, e_mid) && !near(e, e_mid + 300) && !near(e, patch_e) &&
                     !near(e, pe) && !near(n, patch_n) && !near(n, ps);
        const auto rgb = [](std::array<std::uint8_t, 3> c) {
            return std::array<double, 3>{static_cast<double>(c[0]), static_cast<double>(c[1]),
                                         static_cast<double>(c[2])};
        };
        if (in_patch) {
            // Pixel coordinates, centres at whole numbers.
            const double x = (e - patch_e) / kPixel - 0.5;
            const double y = (patch_n - n) / kPixel - 0.5;
            clear = clear && x > 0.0 && y > 0.0 && x < 99.0 && y < 99.0;
            return {{50.0 + x, 50.0 + y, 128.0}, clear};
        }
        if (e < e_mid) {
            return {rgb(kRed), clear};
        }
        if (e < e_mid + 300) {
            return {rgb(kGreen), clear};
        }
        return {rgb(kBlue), clear};
    }
};

ImageryImage read_imagery(const fs::path& db, const TileKey& key) {
    const TileResult r = read_tile(db / tile_path(key), key);
    EXPECT_TRUE(r.tile.has_value()) << to_string(key) << ": " << r.detail;
    const TileLayer* layer = r.tile ? r.tile->find(kImageryLayer) : nullptr;
    EXPECT_NE(layer, nullptr) << to_string(key);
    EXPECT_TRUE(r.tile && r.tile->find(kElevationLayer) != nullptr) << to_string(key);
    auto image = layer != nullptr ? decode_imagery(*layer) : std::nullopt;
    EXPECT_TRUE(image.has_value()) << to_string(key);
    return image.value_or(ImageryImage{});
}

double psnr(const Rgba8Image& a, const Rgba8Image& b) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.texels.size(); ++i) {
        if (i % 4 != 3) {
            const double d = static_cast<double>(a.texels[i]) - static_cast<double>(b.texels[i]);
            sum += d * d;
            ++n;
        }
    }
    return sum == 0.0 ? 1e9 : 10.0 * std::log10(255.0 * 255.0 * static_cast<double>(n) / sum);
}

class CompilerImagery : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        GDALAllRegister();
        scene_ = std::make_unique<ImageryScene>("imagery");
        result_ = std::make_unique<CompileResult>(scene_->build(scene_->imagery_settings()));
    }
    static void TearDownTestSuite() {
        result_.reset();
        scene_.reset();
    }
    static fs::path db() { return scene_->dir / "db"; }
    static std::unique_ptr<ImageryScene> scene_;
    static std::unique_ptr<CompileResult> result_;
};
std::unique_ptr<ImageryScene> CompilerImagery::scene_;
std::unique_ptr<CompileResult> CompilerImagery::result_;

TEST_F(CompilerImagery, EveryTileHasItsTextureAndTheManifestSaysSo) {
    ASSERT_TRUE(result_->ok) << (result_->errors.empty() ? "" : result_->errors[0]);
    ASSERT_TRUE(result_->imagery.has_value());
    EXPECT_EQ(result_->imagery->tiles, 2U + 8U);
    std::unique_ptr<TerrainDatabase> opened;
    const auto r = TerrainDatabase::open(db(), opened);
    ASSERT_TRUE(r.manifest.has_value());
    ASSERT_TRUE(r.manifest->imagery.has_value());
    EXPECT_EQ(r.manifest->imagery->format, ImageryFormat::rgba8_srgb);
    EXPECT_EQ(r.manifest->imagery->texels, 64U);
    for (const LevelTiles& level : r.manifest->levels) {
        for (std::uint32_t x = level.x0; x <= level.x1; ++x) {
            for (std::uint32_t y = level.y0; y <= level.y1; ++y) {
                const ImageryImage image = read_imagery(db(), {level.level, x, y});
                EXPECT_EQ(image.size, 64U);
                EXPECT_EQ(image.mips.size(), 7U);
            }
        }
    }
}

// Each finest texel is its centre's colour: the patch's gradient to within
// rounding (so texel centres and row order are right), the group order, and
// within a group the file the point is deepest inside.
TEST_F(CompilerImagery, FinestTexelsAreTheSourcesAtTheirCentres) {
    ASSERT_TRUE(result_->ok);
    const LevelTiles& fine = result_->manifest.levels[1];
    std::array<int, 4> seen{};  // patch, red, green, blue
    int wrong = 0;
    for (std::uint32_t x = fine.x0; x <= fine.x1; ++x) {
        for (std::uint32_t y = fine.y0; y <= fine.y1; ++y) {
            const TileKey key{16, x, y};
            const Rgba8Image got = decode_mip(read_imagery(db(), key), 0);
            const GeoBounds b = tile_bounds(key);
            const double step = (b.east_deg - b.west_deg) / got.size;
            for (std::uint32_t r = 0; r < got.size; ++r) {
                for (std::uint32_t c = 0; c < got.size; ++c) {
                    const Nad83Point p = scene_->datum.to_nad83(b.south_deg + (r + 0.5) * step,
                                                                b.west_deg + (c + 0.5) * step);
                    const auto [want, clear] = scene_->expected(p.lat_deg, p.lon_deg, 9.0);
                    if (!clear) {
                        continue;
                    }
                    const std::uint8_t* t = got.at(r, c);
                    bool ok = true;
                    for (std::size_t ch = 0; ch < 3; ++ch) {
                        ok = ok && std::abs(static_cast<double>(t[ch]) - want[ch]) <= 0.5 + 1e-9;
                    }
                    wrong += ok ? 0 : 1;
                    if (!ok && wrong < 5) {
                        ADD_FAILURE() << to_string(key) << " texel " << r << ", " << c << ": "
                                      << int{t[0]} << " " << int{t[1]} << " " << int{t[2]}
                                      << ", want " << want[0] << " " << want[1] << " " << want[2];
                    }
                    const std::size_t kind = want[2] == 128.0   ? 0
                                             : want[0] == 200.0 ? 1
                                             : want[1] == 200.0 ? 2
                                                                : 3;
                    ++seen[kind];
                }
            }
        }
    }
    EXPECT_EQ(wrong, 0);
    for (const int n : seen) {
        EXPECT_GT(n, 50);  // every source shows
    }
}

// A coarser tile is its children's halves: each child's colours in its
// quarter (south-west first, rows from the south), averaged in linear light.
TEST_F(CompilerImagery, CoarseTilesAreTheirChildrenHalved) {
    ASSERT_TRUE(result_->ok);
    const LevelTiles& roots = result_->manifest.levels[0];
    for (std::uint32_t x = roots.x0; x <= roots.x1; ++x) {
        const TileKey key{15, x, roots.y0};
        const Rgba8Image got = decode_mip(read_imagery(db(), key), 0);
        const auto kids = children(key);
        std::array<Rgba8Image, 4> halves;
        for (std::size_t i = 0; i < kids.size(); ++i) {
            halves[i] = decode_mip(read_imagery(db(), kids[i]), 1);
        }
        const std::uint32_t h = got.size / 2;
        for (std::uint32_t r = 0; r < got.size; ++r) {
            for (std::uint32_t c = 0; c < got.size; ++c) {
                const Rgba8Image& q = halves[(r >= h ? 2U : 0U) + (c >= h ? 1U : 0U)];
                const std::uint8_t* want = q.at(r % h, c % h);
                const std::uint8_t* t = got.at(r, c);
                ASSERT_TRUE(std::equal(t, t + 4, want)) << to_string(key) << " " << r << ", " << c;
            }
        }
        // And a child's half is its own mip 1: the four texels averaged in
        // linear light (black and white make 188, not 128).
        Rgba8Image bw(2);
        bw.at(0, 0)[0] = bw.at(0, 0)[1] = bw.at(0, 0)[2] = 0;
        bw.at(1, 1)[0] = bw.at(1, 1)[1] = bw.at(1, 1)[2] = 0;
        EXPECT_EQ(int{half(bw).at(0, 0)[0]}, 188);
    }
    // Solid colours stay exact all the way up: the roots' far east is blue.
    const Rgba8Image east = decode_mip(read_imagery(db(), {15, roots.x1, roots.y0}), 0);
    EXPECT_EQ(int{east.at(32, 63)[2]}, 200);
    EXPECT_EQ(int{east.at(32, 63)[0]}, 0);
}

TEST_F(CompilerImagery, CompressedFormatsStayCloseToTheExactOne) {
    ASSERT_TRUE(result_->ok);
    for (const auto& [format, least_db] : {std::pair<const char*, double>{"bc7", 38.0},
                                           std::pair<const char*, double>{"bc1", 26.0}}) {
        const ImageryScene scene(std::string("imagery_") + format);
        const auto r = scene.build(scene.imagery_settings(format));
        ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
        for (const LevelTiles& level : r.manifest.levels) {
            for (std::uint32_t x = level.x0; x <= level.x1; ++x) {
                for (std::uint32_t y = level.y0; y <= level.y1; ++y) {
                    const TileKey key{level.level, x, y};
                    const ImageryImage exact = read_imagery(db(), key);
                    const ImageryImage packed = read_imagery(scene.dir / "db", key);
                    EXPECT_EQ(packed.format, *imagery_format_from_name(format));
                    // Small mips put the scene's hard colour edges into a
                    // few blocks, more than one BC block can hold: looser.
                    for (std::size_t m = 0; m < packed.mips.size(); ++m) {
                        EXPECT_GE(psnr(decode_mip(packed, m), decode_mip(exact, m)),
                                  m < 3 ? least_db : 20.0)
                            << format << " " << to_string(key) << " mip " << m;
                    }
                }
            }
        }
    }
}

TEST(CompilerImageryFailures, AreasTheImageryDoesNotCoverFailAndWriteNothing) {
    GDALAllRegister();
    const ImageryScene scene("imagery_short");
    const auto r = scene.build(scene.imagery_settings("bc7", 64, R"("img/new_a.tif")"));
    ASSERT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].find("imagery samples have no source"), std::string::npos) << r.errors[0];
    EXPECT_FALSE(fs::exists(scene.dir / "db"));
    EXPECT_FALSE(fs::exists(scene.dir / "db.part"));
}

TEST(CompilerImageryFailures, TexelsCoarserThanTheSourcesAreRefused) {
    GDALAllRegister();
    const ImageryScene scene("imagery_coarse");
    const auto r = scene.build(scene.imagery_settings("bc7", 16));
    ASSERT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].find("raise texels"), std::string::npos) << r.errors[0];
}

TEST(CompilerImageryFailures, FinerSourcesNoTexelTakesDoNotCount) {
    GDALAllRegister();
    const ImageryScene scene("imagery_hidden_fine");
    // 1 m imagery everywhere, but last: nothing samples it.
    write_rgb(scene.dir / "cache/img/fine.tif", scene.e_min, scene.n_max, 1.0,
              static_cast<int>(scene.e_max - scene.e_min),
              static_cast<int>(scene.n_max - scene.n_min),
              [](int, int) { return std::array<std::uint8_t, 3>{9, 9, 9}; });
    const auto r = scene.build(scene.imagery_settings(
        "rgba8", 64, R"("img/patch.tif", "img/new_*.tif", "img/old.tif", "img/fine.tif")"));
    EXPECT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
}

TEST(CompilerImageryFailures, SourcesMustBeRgbBytes) {
    GDALAllRegister();
    const ImageryScene scene("imagery_not_rgb");
    // A DEM as imagery: one Float32 band.
    fs::copy_file(scene.dir / "cache/dem/old.tif", scene.dir / "cache/img/dem.tif");
    const auto r = scene.build(scene.imagery_settings("bc7", 64, R"("img/dem.tif")"));
    ASSERT_FALSE(r.ok);
    ASSERT_FALSE(r.errors.empty());
    EXPECT_NE(r.errors[0].find("red, green and blue"), std::string::npos) << r.errors[0];
    EXPECT_EQ(r.errors.size(), 1U);  // once, though every thread found it
}

// Runways (milestone 4 step 1, D-022), fitted to made-up ground given as a
// function of metres along and across a north-south runway (from its south
// end, positive east), so the smooth surface under the noise is known.
constexpr double kRunwayLat = 34.62;
constexpr double kRunwayLon = -86.78;
constexpr double kRunwayLength = 1000.0;
constexpr double kRunwayWidth = 45.72;          // 150 ft
constexpr double kMetresPerDegLat = 110'923.0;  // near 34.6 degrees (WGS-84)
const double kMetresPerDegLon = 91'616.0;       // likewise, at 34.62 degrees

Runway test_runway() {
    Runway r;
    r.name = "36/18";
    r.width_m = kRunwayWidth;
    r.ends[0].id = "36";
    r.ends[0].lat_deg = kRunwayLat;
    r.ends[0].lon_deg = kRunwayLon;
    r.ends[1].id = "18";
    r.ends[1].lat_deg = kRunwayLat + kRunwayLength / kMetresPerDegLat;
    r.ends[1].lon_deg = kRunwayLon;
    return r;
}

struct RunwayGround {
    // Grade changes (0.4% over the length), and a crown.
    std::function<double(double u, double v)> crown = [](double, double v) {
        return -0.015 * std::abs(v);
    };
    double noise_m = 0.03;  // ripples a few metres long, as a survey's noise
    bool bump = true;       // 2.5 m over 20 x 15 m: what stood there in the survey

    double smooth(double u, double v) const {
        // Beyond the sides the shoulders fall away at 4%.
        const double off = std::max(0.0, std::abs(v) - kRunwayWidth / 2);
        return 180.0 + 0.004 * u + 2e-6 * (u - 500.0) * (u - 500.0) +
               crown(u, std::clamp(v, -kRunwayWidth / 2, kRunwayWidth / 2)) - 0.04 * off;
    }
    double ground(double u, double v) const {
        return smooth(u, v) + noise_m * std::sin(0.9 * u + 1.3) * std::sin(1.1 * v + 0.4);
    }
    static double along(double lat) { return (lat - kRunwayLat) * kMetresPerDegLat; }
    static double across(double lon) { return (lon - kRunwayLon) * kMetresPerDegLon; }
    PointHeights heights() const {
        return [this](std::span<const double> lat, std::span<const double> lon,
                      std::vector<float>& out, std::string&) {
            out.resize(lat.size());
            for (std::size_t i = 0; i < lat.size(); ++i) {
                const double u = along(lat[i]);
                const double v = across(lon[i]);
                const bool on_bump = bump && u > 400.0 && u < 420.0 && v > -5.0 && v < 10.0;
                out[i] = static_cast<float>(ground(u, v) + (on_bump ? 2.5 : 0.0));
            }
            return true;
        };
    }
};

// The largest distance from the fit to the smooth ground, over the runway.
double runway_fit_error(const RunwaySurface& s, const RunwayGround& g) {
    double worst = 0.0;
    for (double u = 0.0; u <= kRunwayLength; u += 2.5) {
        for (double v = -kRunwayWidth / 2; v <= kRunwayWidth / 2; v += 1.5) {
            const double lat = kRunwayLat + u / kMetresPerDegLat;
            const double lon = kRunwayLon + v / kMetresPerDegLon;
            const RunwayPoint p = s.to_runway(lat, lon);
            worst = std::max(worst, std::abs(s.surface(p.along_m, p.across_m) - g.smooth(u, v)));
        }
    }
    return worst;
}

TEST(CompilerRunways, ReadsTheFaasDegreesAndMinutes) {
    EXPECT_NEAR(*parse_degrees_minutes("34-39.167130N", true), 34.0 + 39.167130 / 60.0, 1e-12);
    EXPECT_NEAR(*parse_degrees_minutes("086-46.926028W", false), -(86.0 + 46.926028 / 60.0), 1e-12);
    EXPECT_NEAR(*parse_degrees_minutes("12-00.5S", true), -(12.0 + 0.5 / 60.0), 1e-12);
    EXPECT_NEAR(*parse_degrees_minutes("001-30E", false), 1.5, 1e-12);
    for (const char* bad : {"34-39.1N ", "34.5N", "34-61.0N", "91-00.0N", "34-39.1W", "-39.1N",
                            "34-39.1", "34-x.1N", ""}) {
        EXPECT_FALSE(parse_degrees_minutes(bad, true).has_value()) << bad;
    }
    for (const char* bad : {"086-46.9N", "181-00W", "180-00.1W", "086-46.9"}) {
        EXPECT_FALSE(parse_degrees_minutes(bad, false).has_value()) << bad;
    }
}

TEST(CompilerRunways, TheFitFollowsTheGradeAndTheCrownThroughNoiseAndBumps) {
    const RunwayGround tent;
    std::string error;
    const auto s = RunwaySurface::fit(test_runway(), 20.0, tent.heights(), error);
    ASSERT_TRUE(s.has_value()) << error;
    EXPECT_NEAR(s->length_m(), kRunwayLength, 0.5);
    // Within 2 cm of the ground under 3 cm of noise and the bump, which
    // the fit leaves out (2.5 m from it at its worst).
    EXPECT_LT(runway_fit_error(*s, tent), 0.02);
    EXPECT_GT(s->max_abs_m(), 2.3);
    // A rounded crown (edges 0.3 m down) with a cross slope fits as well.
    RunwayGround rounded;
    rounded.crown = [](double, double v) { return -0.0006 * v * v + 0.005 * v; };
    const auto r = RunwaySurface::fit(test_runway(), 20.0, rounded.heights(), error);
    ASSERT_TRUE(r.has_value()) << error;
    EXPECT_LT(runway_fit_error(*r, rounded), 0.02);
    // Without noise or bumps, exactly (to float rounding of the heights).
    RunwayGround clean;
    clean.noise_m = 0.0;
    clean.bump = false;
    const auto c = RunwaySurface::fit(test_runway(), 20.0, clean.heights(), error);
    ASSERT_TRUE(c.has_value()) << error;
    EXPECT_LT(runway_fit_error(*c, clean), 0.002);
}

// A runway's frame back on the Earth (D-033's lights): its points, and its
// true heading where they are, which turns as the meridians converge.
TEST(CompilerRunways, PointsAndHeadingsAreTheRunways) {
    Runway rw = test_runway();
    // 3 km toward the east-north-east.
    rw.ends[1].lat_deg = kRunwayLat + 1000.0 / kMetresPerDegLat;
    rw.ends[1].lon_deg = kRunwayLon + 2800.0 / kMetresPerDegLon;
    const PointHeights flat = [](std::span<const double> lat, std::span<const double>,
                                 std::vector<float>& out, std::string&) {
        out.assign(lat.size(), 180.0F);
        return true;
    };
    std::string error;
    const auto s = RunwaySurface::fit(rw, 20.0, flat, error);
    ASSERT_TRUE(s.has_value()) << error;
    for (const RunwayPoint p :
         {RunwayPoint{0.0, 0.0}, RunwayPoint{1234.5, -30.0}, RunwayPoint{s->length_m(), 22.86}}) {
        const Geodetic g = s->to_geodetic(p);
        const RunwayPoint back = s->to_runway(g.lat_deg, g.lon_deg);
        EXPECT_NEAR(back.along_m, p.along_m, 1e-4);  // set down on the ellipsoid: micrometres
        EXPECT_NEAR(back.across_m, p.across_m, 1e-4);
    }
    const Geodetic first = s->to_geodetic({0.0, 0.0});
    EXPECT_NEAR(first.lat_deg, kRunwayLat, 1e-9);
    EXPECT_NEAR(first.lon_deg, kRunwayLon, 1e-9);
    // Each end's heading: the other end's direction in its own north and
    // east, and the far end's the reverse of the near one's, turned by the
    // meridians' convergence (about 0.017 degrees over 2.8 km east here).
    const auto toward = [](double lat0, double lon0, double lat1, double lon1) {
        const DVec3 d = glm::transpose(ned_to_ecef(lat0, lon0)) *
                        (geodetic_to_ecef({lat1, lon1, 0.0}) - geodetic_to_ecef({lat0, lon0, 0.0}));
        const double h = glm::degrees(std::atan2(d.y, d.x));
        return h < 0.0 ? h + 360.0 : h;
    };
    const RunwayEnd& a = rw.ends[0];
    const RunwayEnd& b = rw.ends[1];
    const double at_a = s->heading_deg(a.lat_deg, a.lon_deg);
    const double at_b = s->heading_deg(b.lat_deg, b.lon_deg);
    EXPECT_NEAR(at_a, toward(a.lat_deg, a.lon_deg, b.lat_deg, b.lon_deg), 1e-3);
    EXPECT_NEAR(at_b, toward(b.lat_deg, b.lon_deg, a.lat_deg, a.lon_deg) - 180.0, 1e-3);
    EXPECT_GT(at_b - at_a, 0.012);
    EXPECT_GT(at_a, 69.0);  // east-north-east
    EXPECT_LT(at_a, 71.0);
}

TEST(CompilerRunways, SomethingAcrossTheWholeRunwayDoesNotStopTheFit) {
    // 3 m high over the full width for 80 m, wider than a station's window:
    // the fit takes it as the ground there (its samples are never all left
    // out, so no station is singular), and is unaffected 60 m away.
    RunwayGround g;
    g.bump = false;
    const PointHeights covered = [&](std::span<const double> lat, std::span<const double> lon,
                                     std::vector<float>& out, std::string& why) {
        (void)g.heights()(lat, lon, out, why);
        for (std::size_t i = 0; i < lat.size(); ++i) {
            const double u = RunwayGround::along(lat[i]);
            if (u > 600.0 && u < 680.0) {
                out[i] += 3.0F;
            }
        }
        return true;
    };
    std::string error;
    const auto s = RunwaySurface::fit(test_runway(), 20.0, covered, error);
    ASSERT_TRUE(s.has_value()) << error;
    // Where the block is, the fit takes it as the ground (it's wider than
    // the window); 60 m away from it, the ground's own.
    double away = 0.0;
    for (double u = 0.0; u <= kRunwayLength; u += 2.5) {
        if (u > 540.0 && u < 740.0) {
            continue;
        }
        const double lat = kRunwayLat + u / kMetresPerDegLat;
        const RunwayPoint p = s->to_runway(lat, kRunwayLon);
        away = std::max(away, std::abs(s->surface(p.along_m, p.across_m) - g.smooth(u, 0.0)));
    }
    EXPECT_LT(away, 0.02);
}

TEST(CompilerRunways, BlendsIntoTheGroundWithNoStep) {
    const RunwayGround g;
    std::string error;
    const auto s = RunwaySurface::fit(test_runway(), 20.0, g.heights(), error);
    ASSERT_TRUE(s.has_value()) << error;
    // Out across the east side (falling away at 4%) and off the south end,
    // in 10 cm steps: the height starts at the runway's surface, keeps the
    // ground's own slope (a correction of centimetres at most, fading out)
    // and is the ground's past the blend.
    const auto line = [&](double u0, double v0, double du, double dv) {
        double last = 0.0;
        double largest_change = 0.0;
        double largest = 0.0;
        for (int i = 1; i <= 250; ++i) {
            const double u = u0 + du * i / 10.0;
            const double v = v0 + dv * i / 10.0;
            const double lat = kRunwayLat + u / kMetresPerDegLat;
            const double lon = kRunwayLon + v / kMetresPerDegLon;
            const double raw = g.ground(u, v);
            const double h = s->blended(lat, lon, raw);
            if (i == 1) {
                const RunwayPoint p = s->to_runway(lat, lon);
                // 10 cm out: the surface, and the ground's own fall and ripple over
                // those 10 cm (4 mm and 3 mm at most).
                EXPECT_NEAR(h, s->surface(p.along_m, p.across_m), 0.01);
            } else {
                largest_change = std::max(largest_change, std::abs((h - raw) - last));
            }
            last = h - raw;
            largest = std::max(largest, std::abs(h - raw));
            if (i == 250) {
                EXPECT_EQ(h, raw);  // 25 m out: past the blend
            }
        }
        EXPECT_LT(largest, 0.04);  // never the edge's height held flat over a falling shoulder
        return largest_change;
    };
    EXPECT_LT(line(500.0, kRunwayWidth / 2, 0.0, 1.0), 0.001);
    EXPECT_LT(line(0.0, 3.0, -1.0, 0.0), 0.001);
}

TEST(CompilerRunways, NeighbouringTilesAgreeAndOthersAreUntouched) {
    const RunwayGround g;
    std::string error;
    const auto s = RunwaySurface::fit(test_runway(), 20.0, g.heights(), error);
    ASSERT_TRUE(s.has_value()) << error;
    const std::vector<RunwaySurface> runways{*s};
    constexpr std::uint32_t n = 33;
    // The level-16 tile under the runway's middle and the one east of it
    // (the runway's east edge and blend cross into it or lie near).
    const TileKey mid = tile_at(kRunwayLat + 500.0 / kMetresPerDegLat, kRunwayLon, 16);
    const TileKey east{16, mid.x + 1, mid.y};
    const TileKey far{16, mid.x + 40, mid.y};
    const auto ground = [&](const TileKey& k) {
        const GeoBounds b = tile_bounds(k);
        const double step = (b.east_deg - b.west_deg) / (n - 1);
        std::vector<float> h(n * n);
        for (std::uint32_t r = 0; r < n; ++r) {
            for (std::uint32_t c = 0; c < n; ++c) {
                h[r * n + c] =
                    static_cast<float>(g.smooth(RunwayGround::along(b.south_deg + r * step),
                                                RunwayGround::across(b.west_deg + c * step)) +
                                       3.0);
            }
        }
        return h;
    };
    std::vector<float> a = ground(mid);
    std::vector<float> b = ground(east);
    const std::vector<float> f0 = ground(far);
    std::vector<float> f = f0;
    const std::vector<float> a0 = a;
    apply_runways(runways, mid, n, a);
    apply_runways(runways, east, n, b);
    apply_runways(runways, far, n, f);
    EXPECT_NE(a, a0);
    for (std::uint32_t r = 0; r < n; ++r) {
        EXPECT_EQ(a[r * n + n - 1], b[r * n]) << "row " << r;
    }
    EXPECT_EQ(f, f0);
}

TEST(CompilerRunways, RefusesBadRunwaysAndMissingHeights) {
    std::string error;
    const RunwayGround g;
    Runway shortest = test_runway();
    shortest.ends[1] = shortest.ends[0];
    EXPECT_FALSE(RunwaySurface::fit(shortest, 20.0, g.heights(), error).has_value());
    EXPECT_NE(error.find("36/18"), std::string::npos) << error;
    const PointHeights none = [](std::span<const double>, std::span<const double>,
                                 std::vector<float>&, std::string& why) {
        why = "no source";
        return false;
    };
    Runway mistyped = test_runway();
    mistyped.length_m = kRunwayLength * 1.01;
    EXPECT_FALSE(RunwaySurface::fit(mistyped, 20.0, g.heights(), error).has_value());
    EXPECT_NE(error.find("apart"), std::string::npos) << error;
    mistyped.length_m = kRunwayLength;
    EXPECT_TRUE(RunwaySurface::fit(mistyped, 20.0, g.heights(), error).has_value()) << error;
    Runway narrow = test_runway();
    narrow.width_m = 4.0;  // too few distances from the centreline for the crown
    EXPECT_FALSE(RunwaySurface::fit(narrow, 20.0, g.heights(), error).has_value());
    narrow.width_m = 5.0;
    EXPECT_TRUE(RunwaySurface::fit(narrow, 20.0, g.heights(), error).has_value()) << error;
    EXPECT_FALSE(RunwaySurface::fit(test_runway(), 20.0, none, error).has_value());
    EXPECT_NE(error.find("no source"), std::string::npos) << error;
}

// Through the compiler, on rough ground: the runway's tiles change, the
// rest don't, neighbours still share their edges and the coarser tiles'
// errors still bound their children.
// Degrees as the FAA writes them: "34-39.167130N".
std::string dm(double deg, bool latitude) {
    const double a = std::abs(deg);
    char text[32];
    (void)std::snprintf(text, sizeof text, latitude ? "%02d-%09.6f%c" : "%03d-%09.6f%c",
                        static_cast<int>(a), (a - std::floor(a)) * 60.0,
                        latitude ? (deg < 0 ? 'S' : 'N') : (deg < 0 ? 'W' : 'E'));
    return std::string(text);
}

TEST(CompilerRunways, TheCompilerSmoothsTheFinestLevelOnly) {
    GDALAllRegister();
    const Scene scene("runways", true);
    const double lat0 = scene.extent.south_deg + 0.001;
    const double lat1 = scene.extent.north_deg - 0.001;
    const double lon = scene.extent.west_deg + 0.0008;  // over the new DEM, clear of its hole
    {
        std::ofstream(scene.dir / "airport.toml")
            << "[[runway]]\nname = \"36/18\"\nwidth_ft = 100.0\n"
            << "[[runway.end]]\nid = \"36\"\nlat = \"" << dm(lat0, true) << "\"\nlon = \""
            << dm(lon, false) << "\"\nelevation_ft = 0.0\n"
            << "[[runway.end]]\nid = \"18\"\nlat = \"" << dm(lat1, true) << "\"\nlon = \""
            << dm(lon, false) << "\"\nelevation_ft = 0.0\n";
    }
    // Two runways crossing are refused (each is fitted alone).
    {
        std::ofstream(scene.dir / "crossing.toml")
            << "[[runway]]\nname = \"36/18\"\nwidth_ft = 100.0\n"
            << "[[runway.end]]\nid = \"36\"\nlat = \"" << dm(lat0, true) << "\"\nlon = \""
            << dm(lon, false) << "\"\nelevation_ft = 0.0\n"
            << "[[runway.end]]\nid = \"18\"\nlat = \"" << dm(lat1, true) << "\"\nlon = \""
            << dm(lon, false) << "\"\nelevation_ft = 0.0\n"
            << "[[runway]]\nname = \"09/27\"\nwidth_ft = 100.0\n"
            << "[[runway.end]]\nid = \"09\"\nlat = \"" << dm((lat0 + lat1) / 2, true)
            << "\"\nlon = \"" << dm(lon - 0.002, false) << "\"\nelevation_ft = 0.0\n"
            << "[[runway.end]]\nid = \"27\"\nlat = \"" << dm((lat0 + lat1) / 2, true)
            << "\"\nlon = \"" << dm(lon + 0.002, false) << "\"\nelevation_ft = 0.0\n";
        const auto x = scene.build(scene.settings() + "[runways]\nairport = \"crossing.toml\"\n");
        EXPECT_FALSE(x.ok);
        ASSERT_FALSE(x.errors.empty());
        EXPECT_NE(x.errors[0].find("crossing"), std::string::npos) << x.errors[0];
    }
    std::string plain = scene.settings();
    plain.replace(plain.find("output = \"db\""), 13, "output = \"plain\"");
    const auto p = scene.build(plain);
    ASSERT_TRUE(p.ok) << (p.errors.empty() ? "" : p.errors[0]);
    const auto r =
        scene.build(scene.settings() + "[runways]\nairport = \"airport.toml\"\nblend_m = 10.0\n");
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    ASSERT_EQ(r.runways.size(), 1U);
    EXPECT_GT(r.runways[0].rms_m, 0.2);  // the rough ground, smoothed away
    const LevelTiles& fine = r.manifest.levels[1];
    int changed = 0;
    int same = 0;
    for (std::uint32_t x = fine.x0; x <= fine.x1; ++x) {
        for (std::uint32_t y = fine.y0; y <= fine.y1; ++y) {
            const ElevationGrid a = read_grid(scene.dir / "db", {16, x, y});
            const ElevationGrid b = read_grid(scene.dir / "plain", {16, x, y});
            const GeoBounds bounds = tile_bounds({16, x, y});
            const double step = (bounds.east_deg - bounds.west_deg) / (a.size - 1);
            for (std::uint32_t row = 0; row < a.size; ++row) {
                for (std::uint32_t c = 0; c < a.size; ++c) {
                    const double metres_off =
                        std::abs(bounds.west_deg + c * step - lon) * kMetresPerDegLon;
                    const bool near = metres_off < 15.24 + 10.0 + 1.0;
                    if (a.at(row, c) != b.at(row, c)) {
                        ++changed;
                        EXPECT_TRUE(near) << "changed " << metres_off << " m from the centreline";
                    } else if (!near) {
                        ++same;
                    }
                }
            }
        }
    }
    EXPECT_GT(changed, 100);
    EXPECT_GT(same, 1000);
    expect_shared_edges(scene.dir / "db", r.manifest);
    // Each root is within its recorded error of its children.
    const LevelTiles& roots = r.manifest.levels[0];
    for (std::uint32_t x = roots.x0; x <= roots.x1; ++x) {
        const TileKey key{15, x, roots.y0};
        const ElevationGrid g = read_grid(scene.dir / "db", key);
        const GeoBounds b = tile_bounds(key);
        double largest = 0.0;
        for (const TileKey& ck : children(key)) {
            const ElevationGrid child = read_grid(scene.dir / "db", ck);
            const GeoBounds cb = tile_bounds(ck);
            const double step = (cb.east_deg - cb.west_deg) / (child.size - 1);
            for (std::uint32_t row = 0; row < child.size; ++row) {
                for (std::uint32_t c = 0; c < child.size; ++c) {
                    const double la = cb.south_deg + row * step;
                    const double lo = cb.west_deg + c * step;
                    largest = std::max(largest, std::abs(static_cast<double>(child.at(row, c)) -
                                                         g.sample(b, la, lo)));
                }
            }
        }
        EXPECT_LE(largest, static_cast<double>(g.error_m) + 1e-3) << to_string(key);
    }
}

TEST(CompilerRunways, AirportFileMistakesAreRefused) {
    const fs::path dir = test_root() / "airport_bad";
    fs::create_directories(dir / "cache" / "dem");
    std::ofstream(dir / "cache" / "dem" / "a.tif") << "x";
    const std::string good =
        "name = \"t\"\narea = { south = 1.0, north = 2.0, west = 3.0, east = 4.0 }\n"
        "output = \"db\"\n[levels]\nroot = 3\nfinest = 5\n"
        "[datum]\nepoch = 2026.0\n[elevation]\nsamples = 65\ncache = \"cache\"\n"
        "sources = [\"dem/a.tif\"]\ngeoid = \"geoid/g.tif\"\n"
        "[runways]\nairport = \"airport.toml\"\n";
    const std::string airport =
        "[[runway]]\nname = \"18R/36L\"\nwidth_ft = 150.0\n"
        "[[runway.end]]\nid = \"18R\"\nlat = \"34-39.167130N\"\nlon = \"086-46.926028W\"\n"
        "elevation_ft = 628.4\n"
        "[[runway.end]]\nid = \"36L\"\nlat = \"34-37.090260N\"\nlon = \"086-46.967450W\"\n"
        "elevation_ft = 611.2\n";
    const auto with = [&](const std::string& from, const std::string& to) {
        std::string t = airport;
        if (!from.empty()) {
            t.replace(t.find(from), from.size(), to);
        }
        std::ofstream(dir / "airport.toml") << t;
        return parse_settings(good, "s.toml", dir);
    };
    const auto ok = with("", "");
    ASSERT_TRUE(ok.settings.has_value()) << (ok.errors.empty() ? "" : ok.errors[0]);
    ASSERT_EQ(ok.settings->runways.size(), 1U);
    const Runway& rw = ok.settings->runways[0];
    EXPECT_EQ(rw.name, "18R/36L");
    EXPECT_NEAR(rw.width_m, 45.72, 1e-9);
    EXPECT_EQ(rw.ends[1].id, "36L");
    EXPECT_NEAR(rw.ends[1].lat_deg, 34.0 + 37.090260 / 60.0, 1e-12);
    EXPECT_NEAR(rw.ends[0].elevation_m, 628.4 * 0.3048, 1e-9);
    EXPECT_DOUBLE_EQ(ok.settings->runway_blend_m, 20.0);
    EXPECT_FALSE(with("34-39.167130N", "34.65N").errors.empty());
    EXPECT_FALSE(with("width_ft = 150.0\n", "").errors.empty());
    EXPECT_FALSE(with("elevation_ft = 611.2\n", "").errors.empty());
    EXPECT_FALSE(with(airport, "").errors.empty());  // no runways
    EXPECT_FALSE(with("[[runway.end]]\nid = \"36L\"", "[[runway.x]]\nid = \"36L\"").errors.empty());
    EXPECT_EQ(with("width_ft = 150.0", "width_ft = = 150.0").errors.size(), 1U);  // not twice
    // The lights' keys (D-033): read, and refused when of the wrong type.
    const auto lit = with("elevation_ft = 611.2\n",
                          "elevation_ft = 611.2\nmarkings = \"PIR\"\nvgsi = \"P4R\"\n"
                          "approach_lights = \"MALSR\"\ncentreline_lights = true\n"
                          "glide_path_deg = 3.0\nthreshold_crossing_height_ft = 72.0\n"
                          "displaced_threshold_ft = 200.0\n");
    ASSERT_TRUE(lit.settings.has_value()) << (lit.errors.empty() ? "" : lit.errors[0]);
    const RunwayEnd& e = lit.settings->runways[0].ends[1];
    EXPECT_EQ(e.markings, "PIR");
    EXPECT_EQ(e.vgsi, "P4R");
    EXPECT_EQ(e.approach_lights, "MALSR");
    EXPECT_TRUE(e.centreline_lights);
    EXPECT_FALSE(e.touchdown_zone_lights);
    EXPECT_DOUBLE_EQ(e.glide_path_deg, 3.0);
    EXPECT_NEAR(e.threshold_crossing_height_m, 72.0 * 0.3048, 1e-12);
    EXPECT_NEAR(e.displaced_threshold_m, 200.0 * 0.3048, 1e-12);
    EXPECT_FALSE(with("width_ft = 150.0\n", "width_ft = 150.0\nedge_lights = 3\n").errors.empty());
    EXPECT_FALSE(
        with("elevation_ft = 611.2\n", "elevation_ft = 611.2\nreil = \"Y\"\n").errors.empty());
    EXPECT_FALSE(with("elevation_ft = 611.2\n", "elevation_ft = 611.2\nglide_path_deg = -3.0\n")
                     .errors.empty());
    // Several airports: a list of files.
    std::ofstream(dir / "airport.toml") << airport;
    {
        std::string second = airport;
        second.replace(second.find("18R/36L"), 7, "17/35");
        std::ofstream(dir / "airport2.toml") << second;
    }
    std::string two = good;
    two.replace(two.find("airport = \"airport.toml\""), 24,
                "airport = [\"airport.toml\", \"airport2.toml\"]");
    const auto both = parse_settings(two, "s.toml", dir);
    ASSERT_TRUE(both.settings.has_value()) << (both.errors.empty() ? "" : both.errors[0]);
    ASSERT_EQ(both.settings->runways.size(), 2U);
    EXPECT_EQ(both.settings->runways[1].name, "17/35");
    std::string none = good;
    none.replace(none.find("airport = \"airport.toml\""), 24, "airport = []");
    EXPECT_FALSE(parse_settings(none, "s.toml", dir).errors.empty());
    std::string twice = good;
    twice.replace(twice.find("airport = \"airport.toml\""), 24,
                  "airport = [\"airport.toml\", \"./airport.toml\"]");
    const auto dup = parse_settings(twice, "s.toml", dir);
    ASSERT_FALSE(dup.errors.empty());
    EXPECT_NE(dup.errors[0].find("listed twice"), std::string::npos) << dup.errors[0];
    // Runways are named with their airport's name, when the file has one.
    std::ofstream(dir / "airport2.toml") << "name = \"KXYZ\"\n" << airport;
    const auto named = parse_settings(two, "s.toml", dir);
    ASSERT_TRUE(named.settings.has_value()) << (named.errors.empty() ? "" : named.errors[0]);
    EXPECT_EQ(named.settings->runways[1].name, "KXYZ 18R/36L");
    fs::remove(dir / "airport2.toml");
    std::string bad_blend = good + "blend_m = -1.0\n";
    EXPECT_FALSE(parse_settings(bad_blend, "s.toml", dir).errors.empty());
    fs::remove(dir / "airport.toml");
    EXPECT_FALSE(parse_settings(good, "s.toml", dir).errors.empty());  // unreadable
}

// Airport lights (milestone 6 step 8, D-033): a runway's layout, baked
// into the root tiles' lights layer, each light on the ground as built,
// facing along the runway. The runway runs north-east across both root
// tiles, one of which also holds a placement.
TEST(CompilerLights, StandOnTheGroundInTheRootTiles) {
    GDALAllRegister();
    const Scene scene("lights", true);
    const double lat0 = scene.extent.south_deg + 0.001;
    const double lat1 = scene.extent.north_deg - 0.001;
    const double mid_lon = (scene.extent.west_deg + scene.extent.east_deg) / 2;  // the tiles' seam
    const double lon0 = mid_lon - 0.003;
    const double lon1 = mid_lon + 0.003;
    const auto write_airport = [&](double north_lat) {
        const auto end = [&](const char* id, double lat, double lon, const std::string& extra) {
            return "[[runway.end]]\nid = \"" + std::string(id) + "\"\nlat = \"" + dm(lat, true) +
                   "\"\nlon = \"" + dm(lon, false) +
                   "\"\nelevation_ft = 0.0\nmarkings = \"PIR\"\ncentreline_lights = true\n" + extra;
        };
        std::ofstream(scene.dir / "airport.toml")
            << "[[runway]]\nname = \"03/21\"\nwidth_ft = 100.0\nedge_lights = \"HIGH\"\n"
            << end("03", lat0, lon0,
                   "vgsi = \"P4L\"\nglide_path_deg = 3.0\nthreshold_crossing_height_ft = 50.0\n"
                   "approach_lights = \"MALSR\"\n")
            << end("21", north_lat, lon1, "");
    };
    write_airport(lat1);
    fs::create_directories(scene.dir / "models");
    const auto box = test::make_glb([] {
        test::GlbSpec g = test::glb_box(2.0F, 2.0F, 2.0F);
        g.extras = R"({"fovea_material": 1})";
        return g;
    }());
    std::ofstream(scene.dir / "models" / "shed.glb", std::ios::binary)
        .write(reinterpret_cast<const char*>(box.data()), static_cast<std::streamsize>(box.size()));
    const double shed_lat = lat0 + 0.001;
    const double shed_lon = lon0 - 0.002;  // in the western tile, with the runway's south end
    std::ofstream(scene.dir / "placed.toml")
        << std::setprecision(12) << "[[placement]]\nmodel = \"shed\"\nlat = " << shed_lat
        << "\nlon = " << shed_lon << "\nheading = 0.0\n";
    const std::string settings =
        scene.settings() +
        "[runways]\nairport = \"airport.toml\"\n[placements]\nfile = \"placed.toml\"\nmodels = "
        "\"models\"\n[lights]\n";
    const auto r = scene.build(settings);
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    ASSERT_TRUE(r.lights.has_value());
    ASSERT_TRUE(r.manifest.lights.has_value());
    EXPECT_EQ(r.manifest.lights->level, Scene::kRoot);
    ASSERT_EQ(r.lights->skipped.size(), 1U);  // the MALSR (FAA JO 6850.2)
    EXPECT_NE(r.lights->skipped[0].find("MALSR"), std::string::npos) << r.lights->skipped[0];

    // Every root tile's lights together are the layout's, each in its
    // tile; both tiles hold some, and the shed's keeps its placement.
    std::vector<TileLight> all;
    std::size_t tiles = 0;
    const LevelTiles& roots = r.manifest.levels[0];
    for (std::uint32_t x = roots.x0; x <= roots.x1; ++x) {
        for (std::uint32_t y = roots.y0; y <= roots.y1; ++y) {
            const TileKey key{Scene::kRoot, x, y};
            const TileResult t = read_tile(scene.dir / "db" / tile_path(key), key);
            ASSERT_TRUE(t.tile.has_value());
            if (const TileLayer* placed = t.tile->find(kPlacementsLayer)) {
                EXPECT_EQ(decode_placements(*placed)->size(), 1U);
                EXPECT_NE(t.tile->find(kLightsLayer), nullptr);
            }
            const TileLayer* layer = t.tile->find(kLightsLayer);
            if (layer == nullptr) {
                continue;
            }
            ++tiles;
            const auto lights = decode_lights(*layer);
            ASSERT_TRUE(lights.has_value());
            for (const TileLight& l : *lights) {
                EXPECT_EQ(tile_at(l.lat_deg, l.lon_deg, Scene::kRoot), key);
                all.push_back(l);
            }
        }
    }
    EXPECT_EQ(tiles, 2U);
    EXPECT_EQ(tiles, r.lights->tiles);
    ASSERT_EQ(all.size(), r.lights->lights);
    const auto loaded = parse_settings(settings, "settings.toml", scene.dir);
    ASSERT_TRUE(loaded.settings.has_value());
    EXPECT_EQ(all.size(),
              layout_lights(loaded.settings->runways[0], r.runways[0].length_m).lights.size());

    // The axis, from the centreline's first and last lights.
    const auto ecef = [](const TileLight& l) {
        return geodetic_to_ecef({l.lat_deg, l.lon_deg, 0.0});
    };
    const TileLight* first = nullptr;
    const TileLight* last = nullptr;
    for (const TileLight& l : all) {
        if (l.type == LightType::centreline) {
            first = first == nullptr || l.lat_deg < first->lat_deg ? &l : first;
            last = last == nullptr || l.lat_deg > last->lat_deg ? &l : last;
        }
    }
    ASSERT_NE(first, nullptr);
    const DVec3 origin = ecef(*first);
    const DVec3 axis = glm::normalize(ecef(*last) - origin);
    const DMat3 to_ned = glm::transpose(ned_to_ecef(first->lat_deg, first->lon_deg));
    std::vector<std::pair<double, float>> papi;  // metres right of the centreline, angle
    for (const TileLight& l : all) {
        // On the finest level's ground.
        const TileKey fine = tile_at(l.lat_deg, l.lon_deg, 16);
        const double ground =
            read_grid(scene.dir / "db", fine).sample(tile_bounds(fine), l.lat_deg, l.lon_deg);
        EXPECT_NEAR(l.height_m, ground, 1e-6);
        // Facing along the axis in its own north and east (from 03, the
        // first end, toward 21): about 55 degrees, turning a little east.
        const DVec3 there = glm::transpose(ned_to_ecef(l.lat_deg, l.lon_deg)) * axis;
        const double heading = glm::degrees(std::atan2(there.y, there.x));
        EXPECT_NEAR(l.heading_deg, heading, 2e-3);
        // Across: right of the axis, level.
        const DVec3 d = to_ned * (ecef(l) - origin);
        const DVec3 u = to_ned * axis;
        const double right = (-u.y * d.x + u.x * d.y) / std::hypot(u.x, u.y);
        if (l.type == LightType::edge) {
            // 10 ft outside the 100 ft pavement (AC 150/5340-30J 2.3.1).
            EXPECT_NEAR(std::abs(right), 60.0 * 0.3048, 0.02);
        } else if (l.type == LightType::centreline) {
            EXPECT_NEAR(right, 0.0, 0.02);
        } else if (l.type == LightType::papi) {
            papi.emplace_back(right, l.papi_angle_deg);
        }
    }
    EXPECT_GT(first->heading_deg, 50.0F);
    EXPECT_LT(first->heading_deg, 60.0F);
    // The PAPI on the left of 03: its highest unit nearest the runway
    // (Table 7-2).
    ASSERT_EQ(papi.size(), 4U);
    std::sort(papi.begin(), papi.end());
    EXPECT_LT(papi.back().first, -15.0);
    for (std::size_t i = 1; i < papi.size(); ++i) {
        EXPECT_GT(papi[i].second, papi[i - 1].second);
    }

    // A runway whose end lights reach past the database is refused.
    write_airport(scene.extent.north_deg - 1e-6);
    const auto past =
        scene.build(scene.settings() + "[runways]\nairport = \"airport.toml\"\n[lights]\n");
    EXPECT_FALSE(past.ok);
    ASSERT_FALSE(past.errors.empty());
    EXPECT_NE(past.errors[0].find("outside the database"), std::string::npos) << past.errors[0];
    EXPECT_FALSE(fs::exists(scene.dir / "db.part"));

    // Lights need runways, the root level for now, and only the keys known.
    const std::string with_runways = scene.settings() + "[runways]\nairport = \"airport.toml\"\n";
    for (const std::string& bad :
         {scene.settings() + "[lights]\n", with_runways + "[lights]\nlevel = 16\n",
          with_runways + "[lights]\nlevel = -1\n", with_runways + "[lights]\nlevel = 15.0\n",
          with_runways + "[lights]\nlevle = 15\n", "lights = true\n" + with_runways}) {
        EXPECT_FALSE(parse_settings(bad, "settings.toml", scene.dir).settings.has_value()) << bad;
    }
    const auto root =
        parse_settings(with_runways + "[lights]\nlevel = 15\n", "settings.toml", scene.dir);
    ASSERT_TRUE(root.settings.has_value()) << root.errors[0];
    EXPECT_EQ(root.settings->lights->level, Scene::kRoot);
}

// Placements (milestone 4 step 4, D-026): a box set at an angle on sloping
// ground sits at the lowest ground under its footprint, in the tile that
// holds it at the root level, its model copied in.
TEST(CompilerPlacements, LandOnTheLowestGroundUnderTheirFootprint) {
    GDALAllRegister();
    const Scene scene("placements");
    fs::create_directories(scene.dir / "models");
    const auto box = test::make_glb([] {
        test::GlbSpec g = test::glb_box(10.0F, 5.0F, 20.0F);
        g.extras = R"({"fovea_material": 1})";
        return g;
    }());
    {
        std::ofstream out(scene.dir / "models" / "shed.glb", std::ios::binary);
        out.write(reinterpret_cast<const char*>(box.data()),
                  static_cast<std::streamsize>(box.size()));
    }
    // Over the newer DEM (plane A + 25 m), clear of its hole and edges.
    const double lat = (scene.extent.south_deg + scene.extent.north_deg) / 2 - 0.0012;
    const double lon = scene.extent.west_deg + 0.0011;
    {
        std::ofstream(scene.dir / "placed.toml")
            << std::setprecision(12) << "[[placement]]\nmodel = \"shed\"\nlat = " << lat
            << "\nlon = " << lon << "\nheading = 30.0\n";
    }
    const auto r = scene.build(scene.settings() +
                               "[placements]\nfile = \"placed.toml\"\nmodels = \"models\"\n");
    ASSERT_TRUE(r.ok) << (r.errors.empty() ? "" : r.errors[0]);
    ASSERT_TRUE(r.placements.has_value());
    EXPECT_EQ(r.placements->placements, 1U);
    ASSERT_TRUE(r.manifest.placements.has_value());
    EXPECT_EQ(r.manifest.placements->level, Scene::kRoot);
    EXPECT_TRUE(fs::exists(scene.dir / "db" / "models" / "shed.glb"));
    const TileKey key = tile_at(lat, lon, Scene::kRoot);
    const TileResult t = read_tile(scene.dir / "db" / tile_path(key), key);
    ASSERT_TRUE(t.tile.has_value());
    const TileLayer* layer = t.tile->find(kPlacementsLayer);
    ASSERT_NE(layer, nullptr);
    const auto placed = decode_placements(*layer);
    ASSERT_TRUE(placed.has_value());
    ASSERT_EQ(placed->size(), 1U);
    EXPECT_EQ((*placed)[0].model, "shed");
    EXPECT_EQ((*placed)[0].heading_deg, 30.0F);
    // The ground is a plane, so its lowest point under the footprint is a
    // corner: 10 m either side across, 20 m front and back, turned 30
    // degrees (the front north-north-east, the left west-north-west).
    // Where it went: the placement file's NAD83(2011), in WGS-84 (D-037).
    double wlat = lat;
    double wlon = lon;
    scene.datum.to_wgs84(wlat, wlon);
    const double h = glm::radians(30.0);
    const DMat3 ned = ned_to_ecef(wlat, wlon);
    double lowest = 1e300;
    for (const double x : {-10.0, 10.0}) {
        for (const double z : {-20.0, 20.0}) {
            const DVec3 front(std::cos(h), std::sin(h), 0.0);
            const DVec3 left(std::sin(h), -std::cos(h), 0.0);
            const Geodetic g = ecef_to_geodetic(geodetic_to_ecef({wlat, wlon, 0.0}) +
                                                ned * (front * z + left * x));
            const auto [height, clear] = scene.ellipsoidal(g.lat_deg, g.lon_deg, 3.0);
            ASSERT_TRUE(clear);
            lowest = std::min(lowest, height);
        }
    }
    EXPECT_NEAR((*placed)[0].height_m, lowest, 0.02);
    // And not the height at its origin: the ground rises across it.
    const auto [at_origin, clear] = scene.ellipsoidal(wlat, wlon, 3.0);
    EXPECT_GT(at_origin - lowest, 0.3);
}

TEST(CompilerPlacements, HeadingsAreTakenIntoZeroTo360) {
    const auto heading = [](const std::string& h) {
        const auto r = parse_placement_file(
            "[[placement]]\nmodel = \"a\"\nlat = 1.0\nlon = 2.0\nheading = " + h + "\n", "p.toml");
        EXPECT_TRUE(r.errors.empty()) << (r.errors.empty() ? "" : r.errors[0]);
        return r.placements.empty() ? -1.0F : r.placements[0].heading_deg;
    };
    EXPECT_FLOAT_EQ(heading("0.9"), 0.9F);
    EXPECT_FLOAT_EQ(heading("-0.9"), 359.1F);
    EXPECT_FLOAT_EQ(heading("360.0"), 0.0F);
    EXPECT_FLOAT_EQ(heading("359.99999999"), 0.0F);  // rounds to 360 as a float
    EXPECT_FLOAT_EQ(heading("725.5"), 5.5F);
    const auto bad = parse_placement_file("[[placement]]\nmodel = \"a\"\nlat = 1.0\n", "p.toml");
    ASSERT_FALSE(bad.errors.empty());
    EXPECT_NE(bad.errors[0].find("p.toml:1"), std::string::npos) << bad.errors[0];
}

TEST(CompilerPlacements, SeveralFilesAddUpAndRepeatsAreRefused) {
    const fs::path dir = test_root() / "placement_files";
    fs::create_directories(dir / "cache" / "dem");
    std::ofstream(dir / "cache" / "dem" / "a.tif") << "x";
    const std::string good =
        "name = \"t\"\narea = { south = 1.0, north = 2.0, west = 3.0, east = 4.0 }\n"
        "output = \"db\"\n[levels]\nroot = 3\nfinest = 5\n"
        "[datum]\nepoch = 2026.0\n[elevation]\nsamples = 65\ncache = \"cache\"\n"
        "sources = [\"dem/a.tif\"]\ngeoid = \"geoid/g.tif\"\n"
        "[placements]\nmodels = \"models\"\n";
    const auto one = [](const std::string& model, double lat) {
        return "[[placement]]\nmodel = \"" + model + "\"\nlat = " + std::to_string(lat) +
               "\nlon = 3.5\nheading = 0.0\n";
    };
    std::ofstream(dir / "a.toml") << one("x", 1.5) << one("y", 1.6);
    std::ofstream(dir / "b.toml") << one("z", 1.7);
    const auto both = parse_settings(good + "file = [\"a.toml\", \"b.toml\"]\n", "s.toml", dir);
    ASSERT_TRUE(both.settings.has_value()) << (both.errors.empty() ? "" : both.errors[0]);
    ASSERT_EQ(both.settings->placements->placements.size(), 3U);  // every file's, in order
    EXPECT_EQ(both.settings->placements->placements[2].model, "z");
    const auto twice = parse_settings(good + "file = [\"a.toml\", \"a.toml\"]\n", "s.toml", dir);
    ASSERT_FALSE(twice.errors.empty());
    EXPECT_NE(twice.errors[0].find("listed twice"), std::string::npos) << twice.errors[0];
    std::ofstream(dir / "b.toml") << one("x", 1.5);  // the same as a.toml's first
    const auto again = parse_settings(good + "file = [\"a.toml\", \"b.toml\"]\n", "s.toml", dir);
    ASSERT_FALSE(again.errors.empty());
    EXPECT_NE(again.errors[0].find("placed there already"), std::string::npos) << again.errors[0];
    EXPECT_NE(parse_settings(good + "file = [\"a.toml\", \"none.toml\"]\n", "s.toml", dir)
                  .errors.front()
                  .find("cannot read"),
              std::string::npos);
}

TEST(CompilerPlacements, MistakesAreRefused) {
    GDALAllRegister();
    const Scene scene("placements_bad");
    fs::create_directories(scene.dir / "models");
    const auto box = test::make_glb([] {
        test::GlbSpec g = test::glb_box(2.0F, 2.0F, 2.0F);
        g.extras = R"({"fovea_material": 1})";
        return g;
    }());
    std::ofstream(scene.dir / "models" / "shed.glb", std::ios::binary)
        .write(reinterpret_cast<const char*>(box.data()), static_cast<std::streamsize>(box.size()));
    std::ofstream(scene.dir / "models" / "junk.glb") << "not a model";
    const double lat = (scene.extent.south_deg + scene.extent.north_deg) / 2;
    const double lon = scene.extent.west_deg + 0.0011;
    const auto with = [&](const std::string& model, double la, double lo, double heading) {
        {
            std::ofstream out(scene.dir / "placed.toml");
            out << std::setprecision(12) << "[[placement]]\nmodel = \"" << model
                << "\"\nlat = " << la << "\nlon = " << lo << "\nheading = " << heading << "\n";
        }
        const auto loaded = parse_settings(
            scene.settings() + "[placements]\nfile = \"placed.toml\"\nmodels = \"models\"\n",
            "settings.toml", scene.dir);
        if (!loaded.settings) {
            return loaded.errors.empty() ? std::string("?") : loaded.errors[0];
        }
        const auto r = compile(*loaded.settings, 2, nullptr);
        EXPECT_FALSE(fs::exists(scene.dir / "db.part"));
        return r.ok ? std::string() : (r.errors.empty() ? std::string("?") : r.errors[0]);
    };
    EXPECT_EQ(with("shed", lat, lon, 0.0), "");
    EXPECT_NE(with("ghost", lat, lon, 0.0).find("ghost"), std::string::npos);
    EXPECT_NE(with("junk", lat, lon, 0.0).find("junk"), std::string::npos);
    EXPECT_NE(with("shed", lat + 1.0, lon, 0.0).find("outside"), std::string::npos);
    EXPECT_NE(with("../shed", lat, lon, 0.0).find("model name"), std::string::npos);
    // Below the roots: the runtime wouldn't draw them yet (#48).
    {
        const auto loaded = parse_settings(scene.settings() +
                                               "[placements]\nfile = \"placed.toml\"\nmodels = "
                                               "\"models\"\nlevel = 16\n",
                                           "settings.toml", scene.dir);
        ASSERT_FALSE(loaded.errors.empty());
        EXPECT_NE(loaded.errors.back().find("root level"), std::string::npos) << loaded.errors[0];
    }
}

TEST(CompilerTexture, MipChainsAndFormatsRoundTrip) {
    Rgba8Image image(16);
    for (std::uint32_t r = 0; r < 16; ++r) {
        for (std::uint32_t c = 0; c < 16; ++c) {
            std::uint8_t* t = image.at(r, c);
            // Colours along a line, as one BC block can hold.
            t[0] = static_cast<std::uint8_t>(40 + c * 8 + r * 4);
            t[1] = static_cast<std::uint8_t>(20 + c * 4 + r * 2);
            t[2] = 90;
        }
    }
    const auto mips = mip_chain(image);
    ASSERT_EQ(mips.size(), 5U);
    EXPECT_EQ(mips.back().size, 1U);
    const ImageryImage exact = encode_texture(mips, ImageryFormat::rgba8_srgb);
    for (std::size_t m = 0; m < mips.size(); ++m) {
        EXPECT_EQ(decode_mip(exact, m).texels, mips[m].texels) << m;
    }
    const ImageryImage bc7 = encode_texture(mips, ImageryFormat::bc7_srgb);
    const ImageryImage bc1 = encode_texture(mips, ImageryFormat::bc1_srgb);
    EXPECT_EQ(bc7.mips.back().size(), 16U);  // the 1 x 1 mip is one block
    EXPECT_EQ(bc1.mips.back().size(), 8U);
    for (std::size_t m = 0; m < mips.size(); ++m) {
        EXPECT_GE(psnr(decode_mip(bc7, m), mips[m]), 40.0) << m;
        EXPECT_GE(psnr(decode_mip(bc1, m), mips[m]), 30.0) << m;
    }
    // A layer encoded in the tile format and back gives the same texels.
    const auto back = decode_imagery(encode_imagery(bc7));
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(decode_mip(*back, 0).texels, decode_mip(bc7, 0).texels);
}

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
        "[datum]\nepoch = 2026.0\n[elevation]\nsamples = 65\ncache = \"cache\"\n"
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
        "[datum]\nepoch = 2026.0\n[elevation]\nsamples = 65\ncache = \"cache\"\n"
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

    fs::create_directories(dir / "cache" / "img");
    std::ofstream(dir / "cache" / "img" / "i.tif") << "x";
    const std::string imagery =
        "[imagery]\nformat = \"bc7\"\ntexels = 512\ncache = \"cache\"\nsources = [\"img/*.tif\"]\n";
    const auto im = parse_settings(good + imagery, "s.toml", dir);
    ASSERT_TRUE(im.settings.has_value()) << (im.errors.empty() ? "" : im.errors[0]);
    ASSERT_TRUE(im.settings->imagery.has_value());
    EXPECT_EQ(im.settings->imagery->format, ImageryFormat::bc7_srgb);
    EXPECT_EQ(im.settings->imagery->texels, 512U);
    ASSERT_EQ(im.settings->imagery->sources.size(), 1U);
    EXPECT_EQ(im.settings->imagery->sources[0].size(), 1U);
    EXPECT_FALSE(parse_settings(good, "s.toml", dir).settings->imagery.has_value());
    const auto imagery_with = [&](const std::string& from, const std::string& to) {
        std::string t = imagery;
        t.replace(t.find(from), from.size(), to);
        return errors(good + t);
    };
    EXPECT_FALSE(imagery_with("\"bc7\"", "\"bc3\"").empty());
    EXPECT_FALSE(imagery_with("512", "500").empty());   // not a power of two
    EXPECT_FALSE(imagery_with("512", "8192").empty());  // past 4096
    EXPECT_FALSE(imagery_with("img/*.tif", "img/none*.tif").empty());
    EXPECT_FALSE(imagery_with("cache = \"cache\"\n", "").empty());

    // Layers don't take files from each other: a file both name is in both.
    std::string both = good + imagery;
    const std::string one = "sources = [\"dem/a.tif\"]";
    both.replace(both.find(one), one.size(), "sources = [\"dem/a.tif\", \"img/*.tif\"]");
    const auto shared = parse_settings(both, "s.toml", dir);
    ASSERT_TRUE(shared.settings.has_value()) << (shared.errors.empty() ? "" : shared.errors[0]);
    EXPECT_EQ(shared.settings->elevation.size(), 2U);
    ASSERT_EQ(shared.settings->imagery->sources.size(), 1U);
    EXPECT_EQ(shared.settings->imagery->sources[0].size(), 1U);
}

}  // namespace
}  // namespace fovea::compiler
