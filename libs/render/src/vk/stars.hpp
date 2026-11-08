#pragma once

// The stars (milestone 6 step 4): the Bright Star Catalogue, drawn as
// single-pixel points into each view's resolved HDR target, behind
// whatever the resolved depth says is nearer than the sky.

#include <volk.h>

#include <vk_mem_alloc.h>

#include "fovea/core/celestial.hpp"
#include "fovea/render/renderer.hpp"
#include "gpu_context.hpp"

namespace fovea {

class StarsGpu {
public:
    // The pipeline draws into `colour` with `depth` to test against; the
    // modules are shaders/stars.slang's vs_stars and fs_stars.
    StarsGpu(const GpuContext& gpu, VkFormat colour, VkFormat depth, VkShaderModule vertex,
             VkShaderModule fragment);
    ~StarsGpu();
    StarsGpu(const StarsGpu&) = delete;
    StarsGpu& operator=(const StarsGpu&) = delete;

    // Draws one view's stars inside a rendering pass on its resolved target
    // and depth, lit by its block of the frame set. No allocation.
    void record(VkCommandBuffer cmd, const SceneView& view, const SkyBodies& sky, VkExtent2D extent,
                std::uint32_t lighting_offset);

private:
    void release();

    GpuContext gpu_;
    VkBuffer stars_ = VK_NULL_HANDLE;
    VmaAllocation stars_memory_ = nullptr;
    std::uint32_t count_ = 0;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace fovea
