#pragma once

// The cloud layers (milestone 6 step 6, D-034): shaders/clouds.slang, one
// full-screen triangle a layer, after the resolve and the stars, into a
// view's HDR target, tested against its resolved depth.

#include <volk.h>

#include <cstdint>
#include <span>

#include "fovea/render/renderer.hpp"
#include "gpu_context.hpp"

namespace fovea {

class CloudsGpu {
public:
    // The pipeline draws into `colour` with `depth` to test against; the
    // modules are shaders/clouds.slang's vs_clouds and fs_clouds.
    CloudsGpu(const GpuContext& gpu, VkFormat colour, VkFormat depth, VkShaderModule vertex,
              VkShaderModule fragment);
    ~CloudsGpu();
    CloudsGpu(const CloudsGpu&) = delete;
    CloudsGpu& operator=(const CloudsGpu&) = delete;

    // Draws one view's cloud layers inside a rendering pass on its resolved
    // target and depth, in `order` (indices into its lighting block's
    // layers, the farthest from the eye's height first). No allocation.
    void record(VkCommandBuffer cmd, const SceneView& view, std::uint32_t lighting_offset,
                std::span<const std::uint32_t> order);

private:
    void release();

    GpuContext gpu_;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace fovea
