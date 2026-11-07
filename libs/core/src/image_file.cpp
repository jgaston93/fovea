#include "fovea/core/image_file.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>

#include <stb_image.h>
#include <stb_image_write.h>

namespace fovea {

namespace {

void append(void* context, void* data, int size) {
    auto* out = static_cast<std::vector<std::uint8_t>*>(context);
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    out->insert(out->end(), bytes, bytes + size);
}

}  // namespace

std::vector<std::uint8_t> encode_png(const RgbaImage& image) {
    std::vector<std::uint8_t> encoded;
    if (image.width == 0 || image.height == 0 ||
        image.pixels.size() != static_cast<std::size_t>(image.width) * image.height * 4 ||
        stbi_write_png_to_func(&append, &encoded, static_cast<int>(image.width),
                               static_cast<int>(image.height), 4, image.pixels.data(),
                               static_cast<int>(image.width) * 4) == 0) {
        return {};
    }
    return encoded;
}

std::vector<std::uint8_t> encode_jpeg(const RgbaImage& image, int quality) {
    std::vector<std::uint8_t> encoded;
    if (image.width == 0 || image.height == 0 ||
        image.pixels.size() != static_cast<std::size_t>(image.width) * image.height * 4 ||
        stbi_write_jpg_to_func(&append, &encoded, static_cast<int>(image.width),
                               static_cast<int>(image.height), 4, image.pixels.data(),
                               std::clamp(quality, 1, 100)) == 0) {
        return {};
    }
    return encoded;
}

bool write_png(const std::filesystem::path& file, const RgbaImage& image) {
    const std::vector<std::uint8_t> encoded = encode_png(image);
    if (encoded.empty()) {
        return false;
    }
    std::ofstream out(file, std::ios::binary);
    out.write(reinterpret_cast<const char*>(encoded.data()),
              static_cast<std::streamsize>(encoded.size()));
    return static_cast<bool>(out);
}

std::optional<RgbaImage> decode_image(std::span<const std::uint8_t> bytes, std::uint32_t max_side) {
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    int w = 0;
    int h = 0;
    int channels = 0;
    // The size first, from the header: a huge image is refused before it
    // is decoded.
    if (stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels) ==
            0 ||
        w <= 0 || h <= 0 || static_cast<std::uint32_t>(w) > max_side ||
        static_cast<std::uint32_t>(h) > max_side) {
        return std::nullopt;
    }
    stbi_uc* data =
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels, 4);
    if (data == nullptr) {
        return std::nullopt;
    }
    RgbaImage image;
    image.width = static_cast<std::uint32_t>(w);
    image.height = static_cast<std::uint32_t>(h);
    image.pixels.assign(data, data + static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    stbi_image_free(data);
    return image;
}

std::optional<RgbaImage> read_png(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }
    const std::streamoff size = in.tellg();
    if (size <= 0 || size > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        return std::nullopt;
    }
    return decode_image(bytes);
}

ImageDifference compare_images(const RgbaImage& a, const RgbaImage& b, std::uint8_t threshold) {
    ImageDifference d;
    d.same_size = a.width == b.width && a.height == b.height &&
                  a.pixels.size() == b.pixels.size() && !a.pixels.empty();
    if (!d.same_size) {
        return d;
    }
    std::uint64_t sum = 0;
    std::size_t over = 0;
    for (std::size_t i = 0; i < a.pixels.size(); i += 4) {
        bool pixel_over = false;
        for (std::size_t c = 0; c < 3; ++c) {
            const int diff = std::abs(int{a.pixels[i + c]} - int{b.pixels[i + c]});
            sum += static_cast<std::uint64_t>(diff);
            d.max_abs = std::max(d.max_abs, static_cast<std::uint8_t>(diff));
            pixel_over = pixel_over || diff > threshold;
        }
        over += pixel_over ? 1 : 0;
    }
    const std::size_t count = a.pixels.size() / 4;
    d.mean_abs = static_cast<double>(sum) / static_cast<double>(count * 3);
    d.fraction_over = static_cast<double>(over) / static_cast<double>(count);
    return d;
}

}  // namespace fovea
