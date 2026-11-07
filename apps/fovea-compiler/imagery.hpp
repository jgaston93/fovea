#pragma once

// Reading imagery sources through GDAL (D-019), as elevation.hpp does for
// heights: rasters in any CRS GDAL reads, with red, green and blue as their
// first three bands, 8 bits each, taken as sRGB (NAIP's fourth band, near
// infrared, isn't used yet).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "fovea/terrain/tile_key.hpp"
#include "texture.hpp"

namespace fovea::compiler {

// One thread's view of the sources (GDAL datasets aren't shared).
class ImagerySources {
public:
    // A group of files per pattern, highest priority first.
    static std::unique_ptr<ImagerySources> open(
        const std::vector<std::vector<std::filesystem::path>>& groups, std::string& error);
    ~ImagerySources();
    ImagerySources(const ImagerySources&) = delete;
    ImagerySources& operator=(const ImagerySources&) = delete;

    // A tile's texture (texels x texels, centres inside the tile): each
    // texel bilinear between pixel centres from the first group with all
    // four pixels around it, and in that group the file the point is
    // deepest inside (furthest from its nearest edge; on a tie, from its
    // nearest edge on the other axis). For the finest level, whose texels must be no more
    // than 1.5 source pixels apart (coarser ones would alias); coarser
    // levels are halved from it (texture.hpp). False, with why, if a texel
    // has no source or the sources are too fine.
    bool texture(const TileKey& key, std::uint32_t texels, Rgba8Image& out,
                 std::string& error) const;

private:
    struct Source;
    ImagerySources() = default;
    std::vector<Source> sources_;  // highest priority first
};

}  // namespace fovea::compiler
