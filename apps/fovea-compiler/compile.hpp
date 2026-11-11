#pragma once

// Builds a terrain database from CompileSettings (milestone 3 step 3):
// the root tiles are those at the root level touching the area, and every
// level down to the finest holds all their descendants, so each tile has
// its four children (D-016). Levels are built finest first: each tile is
// filtered from its children, already written, and its geometric error
// measured against them.
//
// The finest level samples the sources bilinearly and adds the geoid
// (ellipsoidal heights, D-020), with the runways smoothed (D-022, fitted
// first); each coarser level is filtered from the one below. Imagery (D-017) follows, into the same
// tiles: the finest level from the sources, each coarser tile from its children's halves, each with
// its mip chain. The database is built in
// "<output>.part" and moved into place once complete, replacing an older
// database there.

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "fovea/terrain/database.hpp"
#include "lights.hpp"
#include "placements.hpp"
#include "settings.hpp"

namespace fovea::compiler {

struct LevelReport {
    std::uint8_t level = 0;
    std::size_t tiles = 0;
    double seconds = 0.0;
    float max_error_m = 0.0F;  // the largest tile error at this level
    float min_m = 0.0F;        // heights over the level
    float max_m = 0.0F;
};

struct ImageryReport {
    std::size_t tiles = 0;
    double seconds = 0.0;
    std::size_t bytes = 0;  // the imagery layers, mips included
};

struct RunwayReport {
    std::string name;
    double length_m = 0.0;
    double rms_m = 0.0;  // the sources on the runway against the fit
    double max_abs_m = 0.0;
};

struct CompileResult {
    bool ok = false;
    std::vector<std::string> errors;
    DatabaseManifest manifest;
    std::vector<LevelReport> levels;            // in the order built (finest first)
    std::optional<ImageryReport> imagery;       // with [imagery] in the settings
    std::vector<RunwayReport> runways;          // with [runways]
    std::optional<PlacementReport> placements;  // with [placements]
    std::optional<LightsReport> lights;         // with [lights]
};

// The root level's tiles touching the area, as a rectangle.
LevelTiles root_tiles(const GeoBounds& area, std::uint8_t level);
// A rectangle's descendants `levels` levels down.
LevelTiles descendants(const LevelTiles& tiles, std::uint8_t levels);

// `jobs`: worker threads (0: one per hardware thread). `progress` gets a
// line per level.
CompileResult compile(const CompileSettings& settings, unsigned jobs,
                      const std::function<void(const LevelReport&)>& progress);

}  // namespace fovea::compiler
