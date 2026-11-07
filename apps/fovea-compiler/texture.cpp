#include "texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>

#include <bc7decomp.h>
#include <bc7enc.h>
#include <rgbcx.h>

namespace fovea::compiler {

namespace {

// bc7enc and rgbcx each fill tables once before any use.
void init_encoders() {
    static std::once_flag once;
    std::call_once(once, [] {
        bc7enc_compress_block_init();
        rgbcx::init();
    });
}

// The 4 x 4 texels of a block, R first, texels past a small mip's edge
// repeating its last row and column.
std::array<std::uint8_t, 64> block_texels(const Rgba8Image& image, std::uint32_t bx,
                                          std::uint32_t by) {
    std::array<std::uint8_t, 64> out{};
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const std::uint8_t* t = image.at(std::min(by * 4 + y, image.size - 1),
                                             std::min(bx * 4 + x, image.size - 1));
            std::copy_n(t, 4, &out[std::size_t{y * 4 + x} * 4]);
        }
    }
    return out;
}

}  // namespace

Rgba8Image half(const Rgba8Image& image) {
    Rgba8Image out(std::max(1U, image.size / 2));
    const std::uint32_t step = image.size > 1 ? 2 : 1;
    for (std::uint32_t r = 0; r < out.size; ++r) {
        for (std::uint32_t c = 0; c < out.size; ++c) {
            std::uint8_t* o = out.at(r, c);
            for (std::size_t ch = 0; ch < 3; ++ch) {
                float sum = 0.0F;
                for (std::uint32_t i = 0; i < step; ++i) {
                    for (std::uint32_t j = 0; j < step; ++j) {
                        sum += srgb_to_linear(image.at(r * step + i, c * step + j)[ch]);
                    }
                }
                o[ch] = linear_to_srgb(sum / static_cast<float>(step * step));
            }
        }
    }
    return out;
}

Rgba8Image from_children(const Rgba8Image& sw, const Rgba8Image& se, const Rgba8Image& nw,
                         const Rgba8Image& ne) {
    const std::uint32_t h = sw.size;
    Rgba8Image out(h * 2);
    const std::array<std::pair<const Rgba8Image*, std::array<std::uint32_t, 2>>, 4> quarters = {
        {{&sw, {0, 0}}, {&se, {0, h}}, {&nw, {h, 0}}, {&ne, {h, h}}}};
    for (const auto& [child, origin] : quarters) {
        for (std::uint32_t r = 0; r < h; ++r) {
            std::copy_n(child->at(r, 0), std::size_t{h} * 4, out.at(origin[0] + r, origin[1]));
        }
    }
    return out;
}

std::vector<Rgba8Image> mip_chain(Rgba8Image image) {
    std::vector<Rgba8Image> mips;
    mips.push_back(std::move(image));
    while (mips.back().size > 1) {
        mips.push_back(half(mips.back()));
    }
    return mips;
}

ImageryImage encode_texture(const std::vector<Rgba8Image>& mips, ImageryFormat format) {
    init_encoders();
    bc7enc_compress_block_params params;
    bc7enc_compress_block_params_init(&params);  // perceptual (YCbCr) error
    ImageryImage out;
    out.format = format;
    out.size = mips.front().size;
    for (const Rgba8Image& mip : mips) {
        auto& bytes = out.mips.emplace_back(imagery_mip_bytes(format, mip.size));
        if (format == ImageryFormat::rgba8_srgb) {
            std::copy(mip.texels.begin(), mip.texels.end(), bytes.begin());
            continue;
        }
        const std::size_t block_bytes = format == ImageryFormat::bc7_srgb ? 16 : 8;
        const std::uint32_t blocks = (mip.size + 3) / 4;
        for (std::uint32_t by = 0; by < blocks; ++by) {
            for (std::uint32_t bx = 0; bx < blocks; ++bx) {
                const auto texels = block_texels(mip, bx, by);
                std::uint8_t* dst = &bytes[(std::size_t{by} * blocks + bx) * block_bytes];
                if (format == ImageryFormat::bc7_srgb) {
                    bc7enc_compress_block(dst, texels.data(), &params);
                } else {
                    // Level 10 of 18: near the best quality at a fraction
                    // of the time; four colours only (no black-as-alpha).
                    rgbcx::encode_bc1(10, dst, texels.data(), false, false);
                }
            }
        }
    }
    return out;
}

Rgba8Image decode_mip(const ImageryImage& image, std::size_t mip) {
    Rgba8Image out(std::max(1U, image.size >> mip));
    const std::vector<std::uint8_t>& bytes = image.mips[mip];
    if (image.format == ImageryFormat::rgba8_srgb) {
        out.texels = bytes;
        return out;
    }
    const std::size_t block_bytes = image.format == ImageryFormat::bc7_srgb ? 16 : 8;
    const std::uint32_t blocks = (out.size + 3) / 4;
    std::array<std::uint8_t, 64> texels{};
    for (std::uint32_t by = 0; by < blocks; ++by) {
        for (std::uint32_t bx = 0; bx < blocks; ++bx) {
            const std::uint8_t* src = &bytes[(std::size_t{by} * blocks + bx) * block_bytes];
            if (image.format == ImageryFormat::bc7_srgb) {
                bc7decomp::unpack_bc7(src, reinterpret_cast<bc7decomp::color_rgba*>(texels.data()));
            } else {
                rgbcx::unpack_bc1(src, texels.data());
            }
            for (std::uint32_t y = 0; y < 4 && by * 4 + y < out.size; ++y) {
                for (std::uint32_t x = 0; x < 4 && bx * 4 + x < out.size; ++x) {
                    std::copy_n(&texels[std::size_t{y * 4 + x} * 4], 4,
                                out.at(by * 4 + y, bx * 4 + x));
                }
            }
        }
    }
    return out;
}

}  // namespace fovea::compiler
