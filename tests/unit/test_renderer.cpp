#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/render/renderer.hpp"

namespace fovea {
namespace {

// Offscreen rendering needs a Vulkan 1.3 driver but no display: CI runs it on
// lavapipe (Mesa's CPU driver). Machines without any driver skip, unless
// FOVEA_REQUIRE_VULKAN is set (as in CI), where a missing driver fails.
// FOVEA_REQUIRE_VALIDATION (CI, with the layer installed) makes a missing
// validation layer a failure too.
std::unique_ptr<Renderer> make_offscreen(std::vector<OffscreenOutput> outputs) {
    RendererOptions options;
    if (std::getenv("FOVEA_REQUIRE_VALIDATION") != nullptr) {
        options.validation = Validation::required;
    }
    try {
        return Renderer::offscreen(outputs, options);
    } catch (const std::runtime_error& e) {
        if (std::getenv("FOVEA_REQUIRE_VULKAN") != nullptr) {
            ADD_FAILURE() << "no Vulkan: " << e.what();
        }
        return nullptr;
    }
}

#define FOVEA_NEED_VULKAN(renderer)                             \
    if ((renderer) == nullptr) {                                \
        GTEST_SKIP() << "no Vulkan 1.3 driver on this machine"; \
    }

TEST(Renderer, OffscreenClearReadsBackItsColour) {
    auto r = make_offscreen({{64, 32}, {16, 16}});
    FOVEA_NEED_VULKAN(r);
    EXPECT_EQ(r->output_count(), 2U);
    r->draw_clear_frame(std::vector<Rgb8>{{200, 30, 30}, {30, 170, 60}});
    const auto a = r->read_back_rgba(0);
    const auto b = r->read_back_rgba(1);
    ASSERT_EQ(a.size(), 64U * 32U * 4U);
    ASSERT_EQ(b.size(), 16U * 16U * 4U);
    // Every pixel, not just one: the whole image must be cleared.
    int wrong = 0;
    for (std::size_t i = 0; i < a.size(); i += 4) {
        wrong += (a[i] != 200 || a[i + 1] != 30 || a[i + 2] != 30 || a[i + 3] != 255) ? 1 : 0;
    }
    for (std::size_t i = 0; i < b.size(); i += 4) {
        wrong += (b[i] != 30 || b[i + 1] != 170 || b[i + 2] != 60 || b[i + 3] != 255) ? 1 : 0;
    }
    EXPECT_EQ(wrong, 0);
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(Renderer, LaterFramesReplaceEarlierOnes) {
    auto r = make_offscreen({{8, 8}});
    FOVEA_NEED_VULKAN(r);
    for (const std::uint8_t v : std::vector<std::uint8_t>{10, 20, 30, 40, 50}) {
        r->draw_clear_frame(std::vector<Rgb8>{{v, v, v}});
    }
    const auto px = r->read_back_rgba(0);
    EXPECT_EQ(px[0], 50);
    EXPECT_EQ(r->validation_messages(), 0U);
}

TEST(Renderer, ReportsTheGpu) {
    auto r = make_offscreen({{4, 4}});
    FOVEA_NEED_VULKAN(r);
    EXPECT_FALSE(r->gpu().name.empty());
    EXPECT_GE(r->gpu().api_major * 10 + r->gpu().api_minor, 13U);
}

TEST(Renderer, OnlyOneAtATime) {
    auto first = make_offscreen({{4, 4}});
    FOVEA_NEED_VULKAN(first);
    EXPECT_THROW((void)Renderer::offscreen(std::vector<OffscreenOutput>{{4, 4}}),
                 std::runtime_error);
    first.reset();
    EXPECT_NE(make_offscreen({{4, 4}}), nullptr);  // free again once the first is gone
}

TEST(Renderer, ReadBackNeedsAFrameAndAnOffscreenOutput) {
    auto r = make_offscreen({{4, 4}});
    FOVEA_NEED_VULKAN(r);
    EXPECT_THROW((void)r->read_back_rgba(0), std::runtime_error);  // nothing drawn yet
    r->draw_clear_frame(std::vector<Rgb8>{{1, 2, 3}});
    EXPECT_THROW((void)r->read_back_rgba(1), std::runtime_error);
}

}  // namespace
}  // namespace fovea
