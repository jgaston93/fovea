#pragma once

// One terrain tile on disk (D-018): a header naming the tile, a table of
// named layers, and the layers' data. A new kind of layer (materials,
// placements, lights) is a new name in the table, so adding one never
// changes the others or the format around them.
//
// Layout, all little-endian:
//   header (32 bytes): magic "FVTL", schema u16, header size u16, level u8,
//     3 zero bytes, x u32, y u32, layer count u32, 8 zero bytes
//   layer table (48 bytes each): name (16 bytes, NUL-padded), format u32,
//     CRC-32 of the data u32, data offset u64, data size u64, 8 zero bytes
//   layer data, each starting on an 8-byte boundary

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fovea/terrain/tile_key.hpp"

namespace fovea {

inline constexpr std::uint16_t kTileSchema = 1;

struct TileLayer {
    std::string name;  // up to 15 characters
    std::uint32_t format = 0;
    std::vector<std::uint8_t> data;
};

struct TileFile {
    TileKey key;
    std::vector<TileLayer> layers;

    const TileLayer* find(std::string_view name) const;
};

enum class TileError : std::uint8_t {
    none,
    cannot_read,
    too_short,
    bad_magic,
    wrong_schema,  // written by a newer or older Fovea
    wrong_key,     // the file isn't the tile it was asked for
    bad_layer_table,
    checksum,   // a layer's data doesn't match its CRC-32
    bad_layer,  // a layer's data doesn't decode
};

struct TileResult {
    std::optional<TileFile> tile;
    TileError error = TileError::none;
    std::string detail;  // what went wrong, for a message
};

std::vector<std::uint8_t> encode_tile(const TileFile& tile);
// `expected`: the tile the caller asked for; another key is an error.
TileResult decode_tile(std::span<const std::uint8_t> in, const std::optional<TileKey>& expected);

// Writes atomically (a temporary file, then renamed), making directories.
bool write_tile(const std::filesystem::path& file, const TileFile& tile);
TileResult read_tile(const std::filesystem::path& file, const std::optional<TileKey>& expected);

// CRC-32 (IEEE 802.3, the zlib one).
std::uint32_t crc32(std::span<const std::uint8_t> bytes);

// The elevation layer: a square grid of heights above the WGS-84 ellipsoid
// in metres (D-020), row 0 at the tile's south edge, column 0 at its west
// edge, including both edges, so neighbouring tiles share their edge
// samples.
inline constexpr std::string_view kElevationLayer = "elevation";
inline constexpr std::uint32_t kElevationFloat32 = 1;

struct ElevationGrid {
    std::uint32_t size = 0;  // samples per side (257: 256 cells)
    float min_m = 0.0F;      // over the grid
    float max_m = 0.0F;
    float error_m = 0.0F;        // largest height difference from the finest data it stands for
    std::vector<float> heights;  // size * size

    float at(std::uint32_t row, std::uint32_t column) const { return heights[row * size + column]; }
    // Bilinear height at a point in the tile (clamped to its edges).
    double sample(const GeoBounds& bounds, double lat_deg, double lon_deg) const;
};

TileLayer encode_elevation(const ElevationGrid& grid);
std::optional<ElevationGrid> decode_elevation(const TileLayer& layer);

}  // namespace fovea
