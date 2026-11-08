#pragma once

// What the terrain and models need of the renderer's Vulkan objects.

#include <volk.h>

#include <vk_mem_alloc.h>

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
};

}  // namespace fovea
