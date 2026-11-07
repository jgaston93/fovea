#include <cstdio>
#include <filesystem>

#include <gtest/gtest.h>

#include "fovea/core/image_file.hpp"

namespace fovea {
namespace {

RgbaImage gradient(std::uint32_t w, std::uint32_t h) {
    RgbaImage img{w, h, std::vector<std::uint8_t>(static_cast<std::size_t>(w) * h * 4)};
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4;
            img.pixels[i] = static_cast<std::uint8_t>(x);
            img.pixels[i + 1] = static_cast<std::uint8_t>(y);
            img.pixels[i + 2] = static_cast<std::uint8_t>(x + y);
            img.pixels[i + 3] = 255;
        }
    }
    return img;
}

// Files in the test's working directory (its build tree), so runs of
// different builds at once don't share them.
TEST(ImageFile, PngRoundTripIsExact) {
    const std::filesystem::path file = "image_file_round_trip.png";
    const RgbaImage img = gradient(37, 23);
    ASSERT_TRUE(write_png(file, img));
    const auto back = read_png(file);
    std::filesystem::remove(file);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(back->width, 37U);
    EXPECT_EQ(back->height, 23U);
    EXPECT_EQ(back->pixels, img.pixels);
}

TEST(ImageFile, MissingOrBadFilesReadAsNothing) {
    EXPECT_FALSE(read_png("no/such/file.png").has_value());
    const std::filesystem::path file = "image_file_not_a_png.png";
    std::FILE* f = std::fopen(file.string().c_str(), "wb");
    ASSERT_NE(f, nullptr);
    (void)std::fputs("not a png", f);
    std::fclose(f);
    EXPECT_FALSE(read_png(file).has_value());
    std::filesystem::remove(file);
}

TEST(ImageFile, DecodesPngAndJpegInMemoryWithinASize) {
    const RgbaImage img = gradient(40, 24);
    const auto png = decode_image(encode_png(img));
    ASSERT_TRUE(png.has_value());
    EXPECT_EQ(png->pixels, img.pixels);
    const auto jpeg = decode_image(encode_jpeg(img, 95));
    ASSERT_TRUE(jpeg.has_value());
    EXPECT_EQ(jpeg->width, 40U);
    EXPECT_EQ(jpeg->height, 24U);
    EXPECT_LT(compare_images(img, *jpeg, 12).fraction_over, 0.01);  // lossy, but close
    EXPECT_FALSE(decode_image(encode_png(img), 39).has_value());     // wider than allowed
    EXPECT_TRUE(decode_image(encode_png(img), 40).has_value());
    const std::vector<std::uint8_t> junk{1, 2, 3};
    EXPECT_FALSE(decode_image(junk).has_value());
    EXPECT_TRUE(encode_png(RgbaImage{}).empty());
}

TEST(ImageFile, CompareCountsPixelsOverTheThreshold) {
    const RgbaImage a = gradient(10, 10);
    RgbaImage b = a;
    b.pixels[0] = static_cast<std::uint8_t>(b.pixels[0] + 30);  // one pixel, 30 off in red
    b.pixels[4 * 5 + 2] = static_cast<std::uint8_t>(b.pixels[4 * 5 + 2] + 5);  // another, 5 off
    const ImageDifference d = compare_images(a, b, 10);
    EXPECT_TRUE(d.same_size);
    EXPECT_EQ(d.max_abs, 30);
    EXPECT_DOUBLE_EQ(d.fraction_over, 1.0 / 100.0);
    EXPECT_DOUBLE_EQ(d.mean_abs, 35.0 / 300.0);
    EXPECT_FALSE(compare_images(a, gradient(10, 9), 10).same_size);
}

}  // namespace
}  // namespace fovea
