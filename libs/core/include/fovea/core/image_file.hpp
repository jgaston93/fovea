#pragma once

// PNG files for headless dumps and golden-image tests (milestone 1 step 8),
// and PNG or JPEG images in memory (model textures, D-024). Never on the
// render path.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace fovea {

// 8-bit RGBA, rows top to bottom.
struct RgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;  // width * height * 4
};

// Returns false if the file can't be written.
bool write_png(const std::filesystem::path& file, const RgbaImage& image);

// Nothing if the file can't be read or isn't an image.
std::optional<RgbaImage> read_png(const std::filesystem::path& file);

// A PNG or JPEG image in memory, as RGBA (opaque where it has no alpha).
// Nothing if it isn't one, or is larger than `max_side` either way.
std::optional<RgbaImage> decode_image(std::span<const std::uint8_t> bytes,
                                      std::uint32_t max_side = 16384);
// Encoded in memory; empty if the image is malformed. `quality` 1 to 100.
std::vector<std::uint8_t> encode_png(const RgbaImage& image);
std::vector<std::uint8_t> encode_jpeg(const RgbaImage& image, int quality);

// How far apart two images of the same size are (alpha ignored).
struct ImageDifference {
    bool same_size = false;
    double mean_abs = 0.0;       // mean absolute difference per channel, 0..255
    std::uint8_t max_abs = 0;    // largest channel difference
    double fraction_over = 0.0;  // pixels with any channel differing by more than the threshold
};

ImageDifference compare_images(const RgbaImage& a, const RgbaImage& b, std::uint8_t threshold);

}  // namespace fovea
