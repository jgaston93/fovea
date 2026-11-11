#pragma once

// Airport lights (milestone 6 step 8, D-033): the database's lights layer,
// from the root tiles, drawn as points into each view's resolved HDR
// target after the stars, tested against its resolved depth
// (shaders/lights.slang). The lights are placed relative to one origin in
// double on the CPU, so the GPU holds offsets of a few kilometres.

#include <volk.h>

#include <vk_mem_alloc.h>

#include <span>

#include "fovea/core/math.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/terrain/tile_file.hpp"
#include "gpu_context.hpp"

namespace fovea {

class LightsGpu {
public:
    // The pipeline draws into `colour` with `depth` to test against; the
    // modules are shaders/lights.slang's vs_lights and fs_lights. Throws
    // std::runtime_error.
    LightsGpu(const GpuContext& gpu, std::span<const TileLight> lights, VkFormat colour,
              VkFormat depth, VkShaderModule vertex, VkShaderModule fragment);
    ~LightsGpu();
    LightsGpu(const LightsGpu&) = delete;
    LightsGpu& operator=(const LightsGpu&) = delete;

    std::uint32_t count() const { return count_; }

    // Draws one view's lights inside a rendering pass on its resolved
    // target and depth, lit by its block of the frame set. No allocation.
    void record(VkCommandBuffer cmd, const SceneView& view, VkExtent2D extent,
                std::uint32_t lighting_offset);

private:
    void release();

    GpuContext gpu_;
    DVec3 origin_{0.0};  // ECEF: the lights' offsets are from it
    VkBuffer buffer_ = VK_NULL_HANDLE;
    VmaAllocation memory_ = nullptr;
    std::uint32_t count_ = 0;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace fovea
