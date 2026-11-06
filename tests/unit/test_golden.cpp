#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/config.hpp"
#include "fovea/core/image_file.hpp"
#include "fovea/render/renderer.hpp"

namespace fovea {
namespace {

// Golden images: the reference scene for tests/data/golden.toml, compared
// with the PNGs in tests/data/golden. FOVEA_UPDATE_GOLDEN=1 rewrites them
// (on lavapipe, as CI renders) instead of comparing.
//
// GPUs differ in rounding, so the comparison allows small differences:
// kThreshold per channel, on at most kMaxFractionOver of the pixels.
// Measured, the RTX 5070 Ti differs from these lavapipe images by at most 1
// level (mean 0.08 to 0.22); a 0.5 degree yaw change puts several percent
// of pixels over the threshold.
constexpr std::uint8_t kThreshold = 4;
constexpr double kMaxFractionOver = 0.001;

const std::filesystem::path kGoldenDir = FOVEA_SOURCE_DIR "/tests/data/golden";

TEST(Golden, ReferenceSceneMatchesTheGoldenImages) {
    const auto loaded = load_display_config(FOVEA_SOURCE_DIR "/tests/data/golden.toml");
    ASSERT_TRUE(loaded.ok());
    const DisplayConfig& config = *loaded.config;

    std::vector<OffscreenOutput> outputs;
    for (const ChannelConfig& ch : config.channels) {
        outputs.push_back({ch.render_width, ch.render_height});
    }
    const std::vector<SceneView> views = reference_scene_views(config);
    RendererOptions options;
    if (std::getenv("FOVEA_REQUIRE_VALIDATION") != nullptr) {
        options.validation = Validation::required;
    }
    std::unique_ptr<Renderer> r;
    try {
        r = Renderer::offscreen(outputs, options);
    } catch (const std::runtime_error& e) {
        if (std::getenv("FOVEA_REQUIRE_VULKAN") != nullptr) {
            FAIL() << "no Vulkan: " << e.what();
        }
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine";
    }
    r->draw_scene_frame(views);
    EXPECT_EQ(r->validation_messages(), 0U);

    const bool update = std::getenv("FOVEA_UPDATE_GOLDEN") != nullptr;
    // The goldens are lavapipe renders, as CI draws them: no other GPU may
    // write them.
    if (update) {
        ASSERT_NE(r->gpu().name.find("llvmpipe"), std::string::npos)
            << "FOVEA_UPDATE_GOLDEN on " << r->gpu().name << ": set VK_DRIVER_FILES to lavapipe";
    }
    for (std::uint32_t i = 0; i < outputs.size(); ++i) {
        const RgbaImage image{outputs[i].width, outputs[i].height, r->read_back_rgba(i)};
        const auto file = kGoldenDir / (config.channels[i].name + ".png");
        if (update) {
            ASSERT_TRUE(write_png(file, image)) << file;
            continue;
        }
        const auto golden = read_png(file);
        ASSERT_TRUE(golden.has_value())
            << "missing " << file << " (FOVEA_UPDATE_GOLDEN=1 makes it)";
        const ImageDifference d = compare_images(image, *golden, kThreshold);
        ASSERT_TRUE(d.same_size) << file;
        EXPECT_LE(d.fraction_over, kMaxFractionOver)
            << config.channels[i].name << ": " << d.fraction_over * 100.0
            << "% of pixels differ by more than " << int{kThreshold} << " (mean " << d.mean_abs
            << ", max " << int{d.max_abs} << ")";
        // For tuning the tolerance: what this GPU differs by.
        std::printf("golden %-16s mean %.3f  max %3d  over %d: %.4f%%\n",
                    config.channels[i].name.c_str(), d.mean_abs, int{d.max_abs}, int{kThreshold},
                    d.fraction_over * 100.0);
    }
}

}  // namespace
}  // namespace fovea
