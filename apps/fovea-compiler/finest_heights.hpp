#pragma once

// The finest level of a database being built, sampled as the runtime's
// queries would: what placements (D-026) and lights (D-033) stand on.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>

#include "fovea/terrain/tile_file.hpp"
#include "fovea/terrain/tile_key.hpp"

namespace fovea::compiler {

// Heights (ellipsoidal) from the tiles written at `level` under `part`;
// nothing where there is no tile. Each tile is read once.
class FinestHeights {
public:
    FinestHeights(std::filesystem::path part, std::uint8_t level)
        : part_(std::move(part)), level_(level) {}

    std::optional<double> at(double lat_deg, double lon_deg) {
        const TileKey key = tile_at(lat_deg, lon_deg, level_);
        auto found = grids_.find(key);
        if (found == grids_.end()) {
            std::unique_ptr<ElevationGrid> grid;
            const TileResult r = read_tile(part_ / tile_path(key), key);
            const TileLayer* l = r.tile ? r.tile->find(kElevationLayer) : nullptr;
            if (auto g = l != nullptr ? decode_elevation(*l) : std::nullopt) {
                grid = std::make_unique<ElevationGrid>(std::move(*g));
            }
            found = grids_.emplace(key, std::move(grid)).first;
        }
        if (!found->second) {
            return std::nullopt;
        }
        return found->second->sample(tile_bounds(key), lat_deg, lon_deg);
    }

private:
    std::filesystem::path part_;
    std::uint8_t level_;
    std::unordered_map<TileKey, std::unique_ptr<ElevationGrid>> grids_;
};

}  // namespace fovea::compiler
