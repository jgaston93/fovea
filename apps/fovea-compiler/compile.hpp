#pragma once

// Builds a terrain database from CompileSettings (milestone 3 step 3):
// the root tiles are those at the root level touching the area, and every
// level down to the finest holds all their descendants, so each tile has
// its four children (D-016). Levels are built finest first: each tile is
// filtered from its children, already written, and its geometric error
// measured against them.
//
// The finest level samples the sources bilinearly and adds the geoid
// (ellipsoidal heights, D-020); each coarser level is filtered from the one
// below. The database is built in
// "<output>.part" and moved into place once complete, replacing an older
// database there.

#include <functional>
#include <string>
#include <vector>

#include "fovea/terrain/database.hpp"
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

struct CompileResult {
    bool ok = false;
    std::vector<std::string> errors;
    DatabaseManifest manifest;
    std::vector<LevelReport> levels;  // in the order built (finest first)
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
