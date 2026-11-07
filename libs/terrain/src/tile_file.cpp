#include "fovea/terrain/tile_file.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <system_error>

namespace fovea {

namespace {

constexpr std::array<std::uint8_t, 4> kMagic = {'F', 'V', 'T', 'L'};
constexpr std::size_t kHeaderSize = 32;
constexpr std::size_t kEntrySize = 48;
constexpr std::size_t kNameSize = 16;
// More layers than any tile will hold: a damaged count can't make the
// reader allocate gigabytes.
constexpr std::uint32_t kMaxLayers = 256;

// Little-endian on every machine.
template <typename T>
void put(std::vector<std::uint8_t>& out, std::size_t at, T value) {
    auto bytes = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(value);
    if constexpr (std::endian::native == std::endian::big) {
        std::reverse(bytes.begin(), bytes.end());
    }
    std::copy(bytes.begin(), bytes.end(), out.begin() + static_cast<std::ptrdiff_t>(at));
}

template <typename T>
T get(std::span<const std::uint8_t> in, std::size_t at) {
    std::array<std::uint8_t, sizeof(T)> bytes{};
    std::copy_n(in.begin() + static_cast<std::ptrdiff_t>(at), sizeof(T), bytes.begin());
    if constexpr (std::endian::native == std::endian::big) {
        std::reverse(bytes.begin(), bytes.end());
    }
    return std::bit_cast<T>(bytes);
}

std::size_t align8(std::size_t n) {
    return (n + 7U) & ~std::size_t{7};
}

TileResult fail(TileError e, std::string detail) {
    return {std::nullopt, e, std::move(detail)};
}

}  // namespace

const TileLayer* TileFile::find(std::string_view name) const {
    for (const TileLayer& l : layers) {
        if (l.name == name) {
            return &l;
        }
    }
    return nullptr;
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t c = 0xFFFFFFFFU;
    for (const std::uint8_t b : bytes) {
        c = table[(c ^ b) & 0xFFU] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFU;
}

std::vector<std::uint8_t> encode_tile(const TileFile& tile) {
    const auto count = static_cast<std::uint32_t>(tile.layers.size());
    std::size_t size = align8(kHeaderSize + count * kEntrySize);
    for (const TileLayer& l : tile.layers) {
        size = align8(size + l.data.size());
    }
    std::vector<std::uint8_t> out(size, 0);
    std::copy(kMagic.begin(), kMagic.end(), out.begin());
    put(out, 4, kTileSchema);
    put(out, 6, static_cast<std::uint16_t>(kHeaderSize));
    out[8] = tile.key.level;
    put(out, 12, tile.key.x);
    put(out, 16, tile.key.y);
    put(out, 20, count);
    std::size_t offset = align8(kHeaderSize + count * kEntrySize);
    for (std::uint32_t i = 0; i < count; ++i) {
        const TileLayer& l = tile.layers[i];
        const std::size_t entry = kHeaderSize + i * kEntrySize;
        for (std::size_t c = 0; c < std::min(l.name.size(), kNameSize - 1); ++c) {
            out[entry + c] = static_cast<std::uint8_t>(l.name[c]);
        }
        put(out, entry + 16, l.format);
        put(out, entry + 20, crc32(l.data));
        put(out, entry + 24, static_cast<std::uint64_t>(offset));
        put(out, entry + 32, static_cast<std::uint64_t>(l.data.size()));
        std::copy(l.data.begin(), l.data.end(), out.begin() + static_cast<std::ptrdiff_t>(offset));
        offset = align8(offset + l.data.size());
    }
    return out;
}

TileResult decode_tile(std::span<const std::uint8_t> in, const std::optional<TileKey>& expected) {
    if (in.size() < kHeaderSize) {
        return fail(TileError::too_short, "shorter than a tile header");
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), in.begin())) {
        return fail(TileError::bad_magic, "not a Fovea tile (no FVTL magic)");
    }
    const auto schema = get<std::uint16_t>(in, 4);
    if (schema != kTileSchema) {
        return fail(TileError::wrong_schema, "tile schema " + std::to_string(schema) +
                                                 ", this Fovea reads " +
                                                 std::to_string(kTileSchema));
    }
    const auto header = get<std::uint16_t>(in, 6);
    TileFile tile;
    tile.key = {in[8], get<std::uint32_t>(in, 12), get<std::uint32_t>(in, 16)};
    if (!is_valid(tile.key) || (expected && !(tile.key == *expected))) {
        return fail(TileError::wrong_key, "the file holds tile " + to_string(tile.key) +
                                              (expected ? ", not " + to_string(*expected) : ""));
    }
    const auto count = get<std::uint32_t>(in, 20);
    if (header < kHeaderSize || count > kMaxLayers ||
        header + std::size_t{count} * kEntrySize > in.size()) {
        return fail(TileError::bad_layer_table, "the layer table runs past the file");
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t entry = header + i * kEntrySize;
        TileLayer l;
        const auto* name = reinterpret_cast<const char*>(in.data() + entry);
        l.name.assign(name, strnlen(name, kNameSize));
        l.format = get<std::uint32_t>(in, entry + 16);
        const auto crc = get<std::uint32_t>(in, entry + 20);
        const auto offset = get<std::uint64_t>(in, entry + 24);
        const auto size = get<std::uint64_t>(in, entry + 32);
        if (offset > in.size() || size > in.size() - offset) {
            return fail(TileError::bad_layer_table, "layer '" + l.name + "' runs past the file");
        }
        const auto data =
            in.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
        if (crc32(data) != crc) {
            return fail(TileError::checksum, "layer '" + l.name + "' is damaged (CRC-32)");
        }
        l.data.assign(data.begin(), data.end());
        tile.layers.push_back(std::move(l));
    }
    return {std::move(tile), TileError::none, {}};
}

bool write_bytes_atomically(const std::filesystem::path& file,
                            std::span<const std::uint8_t> bytes) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::filesystem::path part = file;
    part += ".part";
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();  // flushes: a full disk shows here, not after the rename
    if (!out) {
        std::filesystem::remove(part, ec);
        return false;
    }
    std::filesystem::rename(part, file, ec);
    if (ec) {
        std::filesystem::remove(part, ec);
        return false;
    }
    return true;
}

bool write_tile(const std::filesystem::path& file, const TileFile& tile) {
    return write_bytes_atomically(file, encode_tile(tile));
}

TileResult read_tile(const std::filesystem::path& file, const std::optional<TileKey>& expected) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        return fail(TileError::cannot_read, "cannot read " + file.string());
    }
    const std::streamoff size = in.tellg();
    std::vector<std::uint8_t> bytes(size > 0 ? static_cast<std::size_t>(size) : 0);
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()))) {
        return fail(TileError::cannot_read, "cannot read " + file.string());
    }
    TileResult r = decode_tile(bytes, expected);
    if (r.error != TileError::none) {
        r.detail = file.string() + ": " + r.detail;
    }
    return r;
}

double ElevationGrid::sample(const GeoBounds& b, double lat_deg, double lon_deg) const {
    const double cells = size - 1;
    const double u =
        std::clamp((lon_deg - b.west_deg) / (b.east_deg - b.west_deg), 0.0, 1.0) * cells;
    const double v =
        std::clamp((lat_deg - b.south_deg) / (b.north_deg - b.south_deg), 0.0, 1.0) * cells;
    const auto c0 = static_cast<std::uint32_t>(std::min(std::floor(u), cells - 1));
    const auto r0 = static_cast<std::uint32_t>(std::min(std::floor(v), cells - 1));
    const double fu = u - c0;
    const double fv = v - r0;
    const auto h = [this](std::uint32_t r, std::uint32_t c) {
        return static_cast<double>(at(r, c));
    };
    const double south = h(r0, c0) * (1.0 - fu) + h(r0, c0 + 1) * fu;
    const double north = h(r0 + 1, c0) * (1.0 - fu) + h(r0 + 1, c0 + 1) * fu;
    return south * (1.0 - fv) + north * fv;
}

TileLayer encode_elevation(const ElevationGrid& g) {
    TileLayer l;
    l.name = std::string(kElevationLayer);
    l.format = kElevationFloat32;
    l.data.assign(16 + g.heights.size() * 4, 0);
    put(l.data, 0, g.size);
    put(l.data, 4, g.min_m);
    put(l.data, 8, g.max_m);
    put(l.data, 12, g.error_m);
    for (std::size_t i = 0; i < g.heights.size(); ++i) {
        put(l.data, 16 + i * 4, g.heights[i]);
    }
    return l;
}

std::optional<ElevationGrid> decode_elevation(const TileLayer& l) {
    if (l.format != kElevationFloat32 || l.data.size() < 16) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> in(l.data);
    ElevationGrid g;
    g.size = get<std::uint32_t>(in, 0);
    if (g.size < 2 || g.size > 8193 || l.data.size() != 16 + std::size_t{g.size} * g.size * 4) {
        return std::nullopt;
    }
    g.min_m = get<float>(in, 4);
    g.max_m = get<float>(in, 8);
    g.error_m = get<float>(in, 12);
    g.heights.resize(std::size_t{g.size} * g.size);
    for (std::size_t i = 0; i < g.heights.size(); ++i) {
        g.heights[i] = get<float>(in, 16 + i * 4);
        if (!std::isfinite(g.heights[i])) {
            return std::nullopt;
        }
    }
    return g;
}

std::string_view imagery_format_name(ImageryFormat f) {
    switch (f) {
        case ImageryFormat::bc7_srgb:
            return "bc7";
        case ImageryFormat::bc1_srgb:
            return "bc1";
        case ImageryFormat::rgba8_srgb:
            return "rgba8";
    }
    return "?";
}

std::optional<ImageryFormat> imagery_format_from_name(std::string_view name) {
    for (const ImageryFormat f :
         {ImageryFormat::bc7_srgb, ImageryFormat::bc1_srgb, ImageryFormat::rgba8_srgb}) {
        if (name == imagery_format_name(f)) {
            return f;
        }
    }
    return std::nullopt;
}

std::size_t imagery_mip_bytes(ImageryFormat f, std::uint32_t size) {
    const std::size_t blocks = (std::size_t{size} + 3) / 4;
    switch (f) {
        case ImageryFormat::bc7_srgb:
            return blocks * blocks * 16;
        case ImageryFormat::bc1_srgb:
            return blocks * blocks * 8;
        case ImageryFormat::rgba8_srgb:
            return std::size_t{size} * size * 4;
    }
    return 0;
}

std::uint32_t imagery_mip_count(std::uint32_t size) {
    return static_cast<std::uint32_t>(std::bit_width(size));
}

TileLayer encode_imagery(const ImageryImage& image) {
    TileLayer l;
    l.name = std::string(kImageryLayer);
    l.format = static_cast<std::uint32_t>(image.format);
    std::size_t bytes = 8;
    for (const auto& mip : image.mips) {
        bytes += mip.size();
    }
    l.data.assign(8, 0);
    l.data.reserve(bytes);
    put(l.data, 0, image.size);
    put(l.data, 4, static_cast<std::uint32_t>(image.mips.size()));
    for (const auto& mip : image.mips) {
        l.data.insert(l.data.end(), mip.begin(), mip.end());
    }
    return l;
}

std::optional<ImageryImage> decode_imagery(const TileLayer& l) {
    const auto format = static_cast<ImageryFormat>(static_cast<std::uint8_t>(l.format));
    if (l.format > 255 || imagery_format_name(format) == "?" || l.data.size() < 8) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> in(l.data);
    ImageryImage image;
    image.format = format;
    image.size = get<std::uint32_t>(in, 0);
    if (image.size < 4 || image.size > 4096 || !std::has_single_bit(image.size) ||
        get<std::uint32_t>(in, 4) != imagery_mip_count(image.size)) {
        return std::nullopt;
    }
    std::size_t at = 8;
    for (std::uint32_t s = image.size; s >= 1; s /= 2) {
        const std::size_t n = imagery_mip_bytes(format, s);
        if (l.data.size() - at < n) {
            return std::nullopt;
        }
        image.mips.emplace_back(l.data.begin() + static_cast<std::ptrdiff_t>(at),
                                l.data.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
    }
    if (at != l.data.size()) {
        return std::nullopt;
    }
    return image;
}

bool valid_model_name(std::string_view name) {
    return !name.empty() && name.size() <= 64 && std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_' || c == '-';
    });
}

bool valid_placement(const Placement& p) {
    // NaNs fail every comparison: each is checked for first.
    return valid_model_name(p.model) && !std::isnan(p.lat_deg) && std::abs(p.lat_deg) <= 90.0 &&
           !std::isnan(p.lon_deg) && std::abs(p.lon_deg) <= 180.0 && std::isfinite(p.height_m) &&
           std::abs(p.height_m) < 1e5 && !std::isnan(p.heading_deg) && p.heading_deg >= 0.0F &&
           p.heading_deg < 360.0F;
}

std::optional<TileLayer> encode_placements(std::span<const Placement> placements) {
    if (!std::all_of(placements.begin(), placements.end(), valid_placement)) {
        return std::nullopt;
    }
    TileLayer l;
    l.name = std::string(kPlacementsLayer);
    l.format = kPlacementsV1;
    l.data.assign(4, 0);
    put(l.data, 0, static_cast<std::uint32_t>(placements.size()));
    for (const Placement& p : placements) {
        const std::size_t at = l.data.size();
        // Name length, name, three doubles, a float, two zero bytes.
        l.data.resize(at + p.model.size() + std::size_t{32}, 0);
        put(l.data, at, static_cast<std::uint16_t>(p.model.size()));
        for (std::size_t k = 0; k < p.model.size(); ++k) {
            l.data[at + 2 + k] = static_cast<std::uint8_t>(p.model[k]);
        }
        std::size_t q = at + 2 + p.model.size();
        put(l.data, q, p.lat_deg);
        put(l.data, q + 8, p.lon_deg);
        put(l.data, q + 16, p.height_m);
        put(l.data, q + 24, p.heading_deg);
    }
    return l;
}

std::optional<std::vector<Placement>> decode_placements(const TileLayer& l) {
    if (l.format != kPlacementsV1 || l.data.size() < 4) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> in(l.data);
    const auto count = get<std::uint32_t>(in, 0);
    std::vector<Placement> out;
    std::size_t at = 4;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (in.size() - at < 2) {
            return std::nullopt;
        }
        const auto length = get<std::uint16_t>(in, at);
        if (in.size() - at - 2 < std::size_t{length} + 30) {
            return std::nullopt;
        }
        Placement p;
        p.model.resize(length);
        for (std::size_t k = 0; k < length; ++k) {
            p.model[k] = static_cast<char>(in[at + 2 + k]);
        }
        const std::size_t q = at + 2 + length;
        p.lat_deg = get<double>(in, q);
        p.lon_deg = get<double>(in, q + 8);
        p.height_m = get<double>(in, q + 16);
        p.heading_deg = get<float>(in, q + 24);
        if (!valid_placement(p) || get<std::uint16_t>(in, q + 28) != 0) {
            return std::nullopt;
        }
        out.push_back(std::move(p));
        at = q + 30;
    }
    if (at != in.size()) {
        return std::nullopt;
    }
    return out;
}

}  // namespace fovea
