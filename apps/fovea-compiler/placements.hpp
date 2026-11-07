#pragma once

// Placements baked into the database (milestone 4 step 4, D-026): each
// landmark in a placement file (a model, a point, a true heading) goes into
// the placements layer of the tile holding it at the placements level, set
// on the lowest ground under its footprint (its model's bounds, turned to
// its heading) in the finest level as built; its model is copied into the
// database's models directory. The compiler reads the models with
// libs/models for their bounds, and so checks them as the runtime will.
//
// A placement file:
//
//   [[placement]]
//   model = "khsv_tower"            # <models dir>/khsv_tower.glb
//   lat = 34.63950                  # degrees, the model's origin
//   lon = -86.77391
//   heading = 0.0                   # degrees clockwise from true north

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "fovea/terrain/database.hpp"
#include "fovea/terrain/tile_file.hpp"

namespace fovea::compiler {

struct PlacementSpec {
    std::string model;
    double lat_deg = 0.0;
    double lon_deg = 0.0;
    float heading_deg = 0.0F;
    std::string where;  // "file:line", for messages
};

struct PlacementFileResult {
    std::vector<PlacementSpec> placements;
    std::vector<std::string> errors;
};
PlacementFileResult load_placement_file(const std::filesystem::path& file);

// What baking them did.
struct PlacementReport {
    std::size_t placements = 0;
    std::size_t models = 0;
    std::size_t tiles = 0;        // that hold some
    double largest_rise_m = 0.0;  // ground under a footprint above its lowest point
    std::string largest_rise_model;
};

// Bakes `placements` into the tiles at `level` in the database being built
// at `part` (its finest level, `finest`, written already), copying their
// models from `models_dir`. Errors name the placement: a model that can't
// be read, a point outside the database.
bool bake_placements(const std::vector<PlacementSpec>& placements,
                     const std::filesystem::path& models_dir, const std::filesystem::path& part,
                     const DatabaseManifest& manifest, std::uint8_t level, std::uint8_t finest,
                     PlacementReport& report, std::vector<std::string>& errors);

}  // namespace fovea::compiler
