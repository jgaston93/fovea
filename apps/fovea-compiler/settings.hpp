#pragma once

// How fovea-compiler builds a database (TOML; data/compile/ has them).
// Relative paths are relative to the settings file.
//
//   name = "khsv"
//   area = "../sources/khsv.toml"   # its [area]; or an inline table
//   output = "../db/khsv"
//   [levels]
//   root = 12                       # the coarsest level: tiles touching the area
//   finest = 16                     # every root's descendants down to here
//   [elevation]
//   samples = 257                   # per tile side, odd, up to 1025
//   cache = "../cache/khsv"         # where tools/fetch-sources.py put the files
//   sources = ["elevation_1m/*_B24.tif", "elevation_10m/*.tif"]  # newest first
//   geoid = "geoid/us_noaa_g2018u0.tif"
//   [imagery]                       # optional: no imagery layer without it
//   format = "bc7"                  # or "bc1", "rgba8" (D-017)
//   texels = 512                    # per tile side, a power of two from 4 to 4096
//   cache = "../cache/khsv"
//   sources = ["imagery/m_*.tif"]   # RGB(+), 8 bits; highest priority first
//   [runways]                       # optional: smoothed in the elevation (D-022)
//   airport = "../airports/khsv.toml"  # its [[runway]]s (format below)
//   blend_m = 20.0                  # the blend into the ground beyond them
//   [[credit]]                      # the manifest's [[source]] entries
//   layer = "elevation"
//   name = "USGS 3DEP 1 m DEM"
//   credit = "U.S. Geological Survey, 3D Elevation Program"
//
// A source pattern is a directory under the cache and a file name with
// '*' wildcards. Where several sources cover a point, the one listed first
// wins; a file matched by two patterns of a layer belongs to the first. For imagery,
// files of one pattern are equals (one year's tiles): the file a point is
// deepest inside wins, so the seams fall midway through their overlaps.
//
// An airport file lists runways as the FAA publishes them:
//
//   [[runway]]
//   name = "18R/36L"
//   width_ft = 150.0
//   length_ft = 12600.0             # optional: checked against the ends
//   [[runway.end]]                  # two
//   id = "18R"
//   lat = "34-39.167130N"           # degrees-minutes and a hemisphere
//   lon = "086-46.926028W"
//   elevation_ft = 628.4            # surveyed, orthometric

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fovea/terrain/database.hpp"
#include "fovea/terrain/tile_key.hpp"
#include "runways.hpp"

namespace fovea::compiler {

struct CompileSettings {
    std::string name;
    GeoBounds area;
    std::filesystem::path output;
    std::uint8_t root_level = 0;
    std::uint8_t finest_level = 0;
    std::uint32_t samples = 257;
    std::vector<std::filesystem::path> elevation;  // files, highest priority first
    std::filesystem::path geoid;
    std::vector<SourceInfo> credits;

    struct Imagery {
        ImageryFormat format = ImageryFormat::bc7_srgb;
        std::uint32_t texels = 512;
        // A group per pattern, highest priority first.
        std::vector<std::vector<std::filesystem::path>> sources;
    };
    std::optional<Imagery> imagery;

    std::vector<Runway> runways;
    double runway_blend_m = 20.0;
};

struct SettingsResult {
    std::optional<CompileSettings> settings;  // set when there are no errors
    std::vector<std::string> errors;          // "file:line: message"
    std::vector<std::string> warnings;        // cache files no pattern names
};

// `base`: the directory relative paths start from.
SettingsResult parse_settings(const std::string& text, const std::string& source,
                              const std::filesystem::path& base);
SettingsResult load_settings(const std::filesystem::path& file);

// '*' matches any run of characters (none included); nothing else is special.
bool wildcard_match(std::string_view pattern, std::string_view name);

}  // namespace fovea::compiler
