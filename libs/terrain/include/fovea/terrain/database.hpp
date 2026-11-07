#pragma once

// A terrain database on disk (D-018): a directory holding manifest.toml and
// tiles/<level>/<x>/<y>.fvt. The manifest names the schema, the area, the
// elevation grid size and, for each level, the rectangle of tiles that
// exist. DatabaseTerrain answers TerrainQuery from a database: the height
// under a point from the finest tile covering it, and ray hits.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "fovea/terrain/terrain_query.hpp"
#include "fovea/terrain/tile_file.hpp"
#include "fovea/terrain/tile_key.hpp"

namespace fovea {

inline constexpr std::uint32_t kDatabaseSchema = 1;

// The tiles of one level that exist: an inclusive rectangle of indices.
struct LevelTiles {
    std::uint8_t level = 0;
    std::uint32_t x0 = 0;
    std::uint32_t x1 = 0;
    std::uint32_t y0 = 0;
    std::uint32_t y1 = 0;

    bool contains(const TileKey& k) const {
        return k.level == level && k.x >= x0 && k.x <= x1 && k.y >= y0 && k.y <= y1;
    }
};

// Where a layer's data came from (D-018), for the record and for credits.
struct SourceInfo {
    std::string layer;   // "elevation"
    std::string name;    // "USGS 3DEP 1 m DEM"
    std::string credit;  // as ATTRIBUTION.md asks
};

// manifest.toml:
//   schema = 1, name = "..."
//   [area] south, north, west, east
//   [layers.elevation] format = "float32", samples = 257
//   [layers.imagery] format = "bc7", texels = 512  (if the database has imagery)
//   [[source]] layer, name, credit          (any number)
//   [[level]] level, x = [first, last], y = [first, last]
// Further layers (the geoid, D-020) add [layers.<name>] tables; a reader
// skips layers it doesn't know.
struct DatabaseManifest {
    std::uint32_t schema = kDatabaseSchema;
    std::string name;
    GeoBounds area;
    std::uint32_t elevation_samples = 257;
    // The format the compiler wrote and the texture size; each tile's layer
    // records its own format (D-017).
    struct Imagery {
        ImageryFormat format = ImageryFormat::bc7_srgb;
        std::uint32_t texels = 512;
    };
    std::optional<Imagery> imagery;
    std::vector<SourceInfo> sources;
    std::vector<LevelTiles> levels;  // coarsest first

    bool has_tile(const TileKey& key) const;
    std::optional<std::uint8_t> finest_level() const;
};

std::string manifest_text(const DatabaseManifest& m);
bool write_manifest(const std::filesystem::path& database_dir, const DatabaseManifest& m);

struct ManifestResult {
    std::optional<DatabaseManifest> manifest;
    std::vector<std::string> errors;  // "file:line: message"
};
ManifestResult parse_manifest(const std::string& text, const std::string& source);

// Where a tile's file lives, relative to the database directory.
std::filesystem::path tile_path(const TileKey& key);

class TerrainDatabase {
public:
    // Reads and checks the manifest; tiles are read when asked for.
    static ManifestResult open(const std::filesystem::path& dir,
                               std::unique_ptr<TerrainDatabase>& out);

    const DatabaseManifest& manifest() const { return manifest_; }
    const std::filesystem::path& dir() const { return dir_; }
    TileResult read(const TileKey& key) const;

private:
    TerrainDatabase(std::filesystem::path dir, DatabaseManifest manifest)
        : dir_(std::move(dir)), manifest_(std::move(manifest)) {}

    std::filesystem::path dir_;
    DatabaseManifest manifest_;
};

// TerrainQuery over a database. Tiles' elevation grids are read on first use
// and kept (a CPU-side cache for hosts and tools, not the renderer's paging).
// Safe to call from several threads.
class DatabaseTerrain final : public TerrainQuery {
public:
    explicit DatabaseTerrain(const TerrainDatabase& db) : db_(db) {}

    // From the finest tile covering the point that reads cleanly; nothing
    // outside the database's tiles.
    std::optional<double> height_at(double lat_deg, double lon_deg) const override;

    // Marches the ray in steps of half its height above the terrain (at
    // least half the finest grid's spacing), then bisects the crossing.
    // Terrain steeper than about 63 degrees between steps can be stepped
    // over; real terrain at these grid spacings isn't.
    std::optional<TerrainHit> intersect(const DRay& ray, double max_distance_m) const override;

    // Tiles that failed to read (damaged, missing), with why: skipped, and
    // the next coarser tile used.
    std::vector<std::string> problems() const;

private:
    const ElevationGrid* grid(const TileKey& key) const;  // nullptr: unreadable

    const TerrainDatabase& db_;
    mutable std::mutex mutex_;
    mutable std::unordered_map<TileKey, std::unique_ptr<ElevationGrid>> grids_;
    mutable std::vector<std::string> problems_;
};

}  // namespace fovea
