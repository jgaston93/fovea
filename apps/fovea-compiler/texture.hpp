#pragma once

// Imagery textures (D-017): sRGB images, their mip chains, and the GPU
// formats they're stored in. No GDAL: the encoders are bc7enc_rdo's
// (cmake/FoveaDependencies.cmake).

#include <cstdint>
#include <vector>

#include "fovea/core/colour.hpp"

#include "fovea/terrain/tile_file.hpp"

namespace fovea::compiler {

// A square sRGB image, 4 bytes a texel (alpha 255), row 0 at the south.
struct Rgba8Image {
    std::uint32_t size = 0;
    std::vector<std::uint8_t> texels;  // size * size * 4

    explicit Rgba8Image(std::uint32_t s = 0) : size(s), texels(std::size_t{s} * s * 4, 255) {}
    std::uint8_t* at(std::uint32_t row, std::uint32_t column) {
        return &texels[(std::size_t{row} * size + column) * 4];
    }
    const std::uint8_t* at(std::uint32_t row, std::uint32_t column) const {
        return &texels[(std::size_t{row} * size + column) * 4];
    }
};

// Half the size: each texel the average of four, in linear light.
Rgba8Image half(const Rgba8Image& image);
// A tile's image from its four children's halves (mip 1 of each): each
// in its quarter.
Rgba8Image from_children(const Rgba8Image& sw, const Rgba8Image& se, const Rgba8Image& nw,
                         const Rgba8Image& ne);
// The image and its halves down to 1 x 1.
std::vector<Rgba8Image> mip_chain(Rgba8Image image);

// Encodes a mip chain. Safe from several threads.
ImageryImage encode_texture(const std::vector<Rgba8Image>& mips, ImageryFormat format);
// One mip back to texels (bc7decomp and rgbcx's decoders), for tests and
// checks.
Rgba8Image decode_mip(const ImageryImage& image, std::size_t mip);

}  // namespace fovea::compiler
