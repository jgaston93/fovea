#include "fovea/terrain/tile_key.hpp"

#include <algorithm>
#include <cmath>

namespace fovea {

double tile_size_deg(std::uint8_t level) {
    return std::ldexp(180.0, -int{level});  // exact: a power of two times 180
}

std::uint32_t tile_columns(std::uint8_t level) {
    return std::uint32_t{2} << level;
}

std::uint32_t tile_rows(std::uint8_t level) {
    return std::uint32_t{1} << level;
}

bool is_valid(const TileKey& key) {
    return key.level <= kMaxTileLevel && key.x < tile_columns(key.level) &&
           key.y < tile_rows(key.level);
}

GeoBounds tile_bounds(const TileKey& key) {
    const double size = tile_size_deg(key.level);
    GeoBounds b;
    b.west_deg = -180.0 + key.x * size;
    b.east_deg = b.west_deg + size;
    b.south_deg = -90.0 + key.y * size;
    b.north_deg = b.south_deg + size;
    return b;
}

TileKey tile_at(double lat_deg, double lon_deg, std::uint8_t level) {
    const double size = tile_size_deg(level);
    const auto index = [size](double offset, std::uint32_t count) {
        const double i = std::floor(offset / size);
        return static_cast<std::uint32_t>(std::clamp(i, 0.0, static_cast<double>(count - 1)));
    };
    return {level, index(lon_deg + 180.0, tile_columns(level)),
            index(lat_deg + 90.0, tile_rows(level))};
}

std::optional<TileKey> parent(const TileKey& key) {
    if (key.level == 0) {
        return std::nullopt;
    }
    return TileKey{static_cast<std::uint8_t>(key.level - 1), key.x / 2, key.y / 2};
}

std::array<TileKey, 4> children(const TileKey& key) {
    const auto level = static_cast<std::uint8_t>(key.level + 1);
    const std::uint32_t x = key.x * 2;
    const std::uint32_t y = key.y * 2;
    return {TileKey{level, x, y}, TileKey{level, x + 1, y}, TileKey{level, x, y + 1},
            TileKey{level, x + 1, y + 1}};
}

std::optional<TileKey> neighbour(const TileKey& key, int dx, int dy) {
    const auto columns = static_cast<std::int64_t>(tile_columns(key.level));
    const auto rows = static_cast<std::int64_t>(tile_rows(key.level));
    const std::int64_t y = std::int64_t{key.y} + dy;
    if (y < 0 || y >= rows) {
        return std::nullopt;
    }
    std::int64_t x = (std::int64_t{key.x} + dx) % columns;
    if (x < 0) {
        x += columns;
    }
    return TileKey{key.level, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)};
}

std::string to_string(const TileKey& key) {
    return std::to_string(key.level) + "/" + std::to_string(key.x) + "/" + std::to_string(key.y);
}

}  // namespace fovea
