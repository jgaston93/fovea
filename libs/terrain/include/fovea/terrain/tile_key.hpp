#pragma once

// Tile addresses in the terrain database's geographic quadtree (D-015).
// Level 0 is two tiles, the western and eastern hemispheres; each level
// halves a tile's latitude and longitude, so a tile at level L spans
// 180 / 2^L degrees each way. x counts columns eastward from 180 W, y rows
// northward from the south pole: level L has 2^(L+1) columns and 2^L rows.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace fovea {

struct GeoBounds {
    double south_deg = 0.0;
    double north_deg = 0.0;
    double west_deg = 0.0;
    double east_deg = 0.0;
};

struct TileKey {
    std::uint8_t level = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;

    friend bool operator==(const TileKey&, const TileKey&) = default;
};

// Deep enough for centimetre tiles; a level's columns still fit in 32 bits.
inline constexpr std::uint8_t kMaxTileLevel = 30;

// A tile's side in degrees: 180 / 2^level.
double tile_size_deg(std::uint8_t level);
std::uint32_t tile_columns(std::uint8_t level);  // 2^(level + 1)
std::uint32_t tile_rows(std::uint8_t level);     // 2^level

// True if the key names a tile that exists (level and indices in range).
bool is_valid(const TileKey& key);

GeoBounds tile_bounds(const TileKey& key);

// The tile at `level` containing a point. Points on an edge belong to the
// tile to their north and east; the north pole and 180 E belong to the last
// row and column. Longitudes outside -180..180 wrap; a non-finite point
// gives tile 0, 0 (check before asking).
TileKey tile_at(double lat_deg, double lon_deg, std::uint8_t level);

// A longitude in [-180, 180), except 180 itself, which stays 180 (the
// eastern edge of the last column).
double wrap_longitude(double lon_deg);

std::optional<TileKey> parent(const TileKey& key);  // nothing at level 0

// South-west, south-east, north-west, north-east.
std::array<TileKey, 4> children(const TileKey& key);

// The tile dx columns east and dy rows north at the same level: columns wrap
// around the antimeridian; rows past a pole don't exist.
std::optional<TileKey> neighbour(const TileKey& key, int dx, int dy);

std::string to_string(const TileKey& key);  // "level/x/y"

}  // namespace fovea

template <>
struct std::hash<fovea::TileKey> {
    std::size_t operator()(const fovea::TileKey& k) const noexcept {
        return std::hash<std::uint64_t>{}((std::uint64_t{k.level} << 58) ^
                                          (std::uint64_t{k.x} << 29) ^ std::uint64_t{k.y});
    }
};
