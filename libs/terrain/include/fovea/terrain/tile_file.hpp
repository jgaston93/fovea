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

// Writes a file atomically (a temporary file, flushed and closed, then
// renamed over it), making directories; on any failure the old file stays
// and nothing is left behind.
bool write_bytes_atomically(const std::filesystem::path& file, std::span<const std::uint8_t> bytes);
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

// The imagery layer (D-017): a square texture covering the tile, texel
// centres inside it (texel (0, 0) is the south-west corner's), with its
// full mip chain down to 1 x 1, ready to upload: row 0 at the tile's
// south edge, as the elevation grid. Colours are sRGB. The layer's format
// is the texture format, so tiles of one database can differ.
//   data: size u32 (texels per side of mip 0, a power of two from 4 to
//   4096), mip count u32, then each mip in turn, largest first (BC formats
//   as 4 x 4 blocks in rows, a mip smaller than 4 x 4 as one block).
inline constexpr std::string_view kImageryLayer = "imagery";

enum class ImageryFormat : std::uint8_t {  // the layer's format number
    bc7_srgb = 1,                          // 16 bytes per 4 x 4 block
    bc1_srgb = 2,                          // 8 bytes per block, no alpha
    rgba8_srgb = 3,                        // uncompressed, alpha 255
};

// "bc7", "bc1", "rgba8".
std::string_view imagery_format_name(ImageryFormat format);
std::optional<ImageryFormat> imagery_format_from_name(std::string_view name);
// The bytes of one mip `size` texels across.
std::size_t imagery_mip_bytes(ImageryFormat format, std::uint32_t size);
std::uint32_t imagery_mip_count(std::uint32_t size);  // down to 1 x 1

struct ImageryImage {
    ImageryFormat format = ImageryFormat::bc7_srgb;
    std::uint32_t size = 0;                       // mip 0, texels per side
    std::vector<std::vector<std::uint8_t>> mips;  // largest first, the full chain
};

TileLayer encode_imagery(const ImageryImage& image);
std::optional<ImageryImage> decode_imagery(const TileLayer& layer);

// The placements layer (D-026): models placed in the tile, each by name
// (a file in the database's models directory, models/<name>.glb), at a
// point and true heading, its ground line at an ellipsoidal height (the
// lowest terrain under its footprint, as the compiler found it).
//   data: count u32, then each: name length u16, name (that many bytes),
//   latitude f64, longitude f64, height f64 (metres above the WGS-84
//   ellipsoid), heading f32 (degrees clockwise from true north), 2 zero
//   bytes. Names are 1 to 64 of A-Z a-z 0-9 _ -.
inline constexpr std::string_view kPlacementsLayer = "placements";
inline constexpr std::uint32_t kPlacementsV1 = 1;

struct Placement {
    std::string model;
    double lat_deg = 0.0;
    double lon_deg = 0.0;
    double height_m = 0.0;
    float heading_deg = 0.0F;
};

bool valid_model_name(std::string_view name);
// A name valid_model_name takes, a point on the Earth, a finite height
// within 100 km of the ellipsoid, a heading from 0 up to 360.
bool valid_placement(const Placement& p);
// Nothing if any placement isn't valid (the reader would refuse the layer).
std::optional<TileLayer> encode_placements(std::span<const Placement> placements);
// Nothing if malformed: a bad name, a point or heading out of range, or
// bytes left over.
std::optional<std::vector<Placement>> decode_placements(const TileLayer& layer);

}  // namespace fovea
