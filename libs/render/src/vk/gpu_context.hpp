#pragma once

// What the terrain and models need of the renderer's Vulkan objects.

#include <volk.h>

#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>

namespace fovea {

struct GpuContext {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = nullptr;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    bool bc_textures = false;     // textureCompressionBC enabled
    float max_anisotropy = 0.0F;  // 0: samplerAnisotropy not enabled
    // Signalled by every frame's submit with the frame's number plus one:
    // the paging thread reads it to reuse what a frame's uploads used.
    VkSemaphore timeline = VK_NULL_HANDLE;
    // The scene pass's samples per pixel (milestone 5, D-027): the terrain
    // and model pipelines draw into it.
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    // Each view's lighting (milestone 6, D-029): a ViewLighting block per
    // frame slot and output, in one uniform buffer. Pipelines that light
    // take this layout as their set 1, bound with the view's offset.
    VkDescriptorSetLayout frame_layout = VK_NULL_HANDLE;
    VkDescriptorSet frame_set = VK_NULL_HANDLE;
};

// What each view's shaders light with: mirrored as ViewLighting in
// shaders/include/frame.slang (std140).
struct ViewLighting {
    std::array<float, 3> sun{0.0F, 0.0F, 1.0F};   // towards the sun, ECEF axes
    float moon_lit = 0.0F;                        // the fraction of the moon's disc lit
    std::array<float, 3> moon{0.0F, 0.0F, 1.0F};  // towards the moon from the eye, ECEF axes
    float sun_up = 1.0F;  // how much of the sun's disc is above the eye's horizon
    std::array<float, 3> sun_local{0.0F, 0.0F, -1.0F};  // the eye's level frame (x north, z down)
    std::uint32_t view = 0;                             // the view's entries in the sky's arrays
    float eye_radius_km = 0.0F;  // the atmosphere's bottom radius plus the eye's height
    float inv_width = 1.0F;      // 1 / the view's render size, pixels
    float inv_height = 1.0F;
    std::uint32_t views = 1;                            // how many views share the sky's arrays
    std::array<float, 3> moon_local{0.0F, 0.0F, 1.0F};  // the eye's level frame
    float moon_up = 0.0F;  // how much of the moon's disc is above the eye's horizon
};
static_assert(sizeof(ViewLighting) == 80, "must match ViewLighting in frame.slang");

}  // namespace fovea
