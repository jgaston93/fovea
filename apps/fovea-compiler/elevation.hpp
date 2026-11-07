#pragma once

// Reading elevation sources through GDAL (D-019): with imagery.cpp, the
// only files in Fovea that see GDAL's API, so the rest of the compiler
// stays plain C++.
//
// Sources are DEMs in any CRS GDAL reads, with orthometric heights (NAVD88
// for 3DEP); the geoid grid turns those into ellipsoid heights (D-020):
// h = H + N. Tile grids are in NAD83 geographic coordinates, which this
// compiler takes as WGS-84: they differ by about a metre here, below the
// accuracy the sources state (docs/milestone-3.md, step 3).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "fovea/terrain/tile_key.hpp"

namespace fovea::compiler {

// Once, before any other GDAL use: registers the drivers.
void init_gdal();

// One thread's view of the sources: GDAL datasets can't be shared between
// threads, so each worker opens its own.
class ElevationSources {
public:
    // `files`: highest priority first.
    static std::unique_ptr<ElevationSources> open(const std::vector<std::filesystem::path>& files,
                                                  std::string& error);
    ~ElevationSources();
    ElevationSources(const ElevationSources&) = delete;
    ElevationSources& operator=(const ElevationSources&) = delete;

    // The source heights at a tile's grid points (samples x samples, row 0
    // at the south edge, edges included): bilinear between pixel centres,
    // from the highest-priority source with all four pixels around the
    // point. False, with why, if a sample has none. For the finest level;
    // coarser ones are filtered from it (compile.cpp).
    //
    // Not GDALWarp: where a source ends within a tile, its warped values
    // (bilinear, and averaged even more) depend on where the warped raster
    // starts, so neighbouring tiles disagreed by centimetres on their
    // shared samples.
    bool heights(const TileKey& key, std::uint32_t samples, std::vector<float>& out,
                 std::string& error) const;

private:
    struct Source;
    ElevationSources() = default;
    std::vector<Source> sources_;  // highest priority first
};

// A geoid grid (geographic, any longitude convention), held for an area and
// sampled bilinearly: metres from the ellipsoid up to the geoid.
class Geoid {
public:
    static std::optional<Geoid> load(const std::filesystem::path& file, const GeoBounds& area,
                                     std::string& error);
    double at(double lat_deg, double lon_deg) const;

private:
    double wrap(double lon_deg) const;
    double origin_lon_ = 0.0;  // the first held column's centre
    double origin_lat_ = 0.0;  // the first held row's centre
    double step_lon_ = 0.0;
    double step_lat_ = 0.0;   // negative for a north-up grid
    double grid_west_ = 0.0;  // query longitudes are wrapped into [this, this + 360)
    int columns_ = 0;
    int rows_ = 0;
    std::vector<float> values_;
};

}  // namespace fovea::compiler
