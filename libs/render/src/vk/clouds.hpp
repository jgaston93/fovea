#pragma once

// The cloud layers (milestone 6 step 6, D-034): shaders/clouds.slang, one
// full-screen triangle a layer, after the resolve and the stars, into a
// view's HDR target, tested against its resolved depth; as surfaces or as
// volumes ([display] clouds). Their noise (shaders/cloud_noise.slang) is
// made once on the GPU, on the first frame.

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstdint>
#include <span>

#include "fovea/render/renderer.hpp"
#include "gpu_context.hpp"

namespace fovea {

// The shaders' modules: clouds.slang's and cloud_noise.slang's.
struct CloudShaders {
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule layer = VK_NULL_HANDLE;   // fs_clouds
    VkShaderModule volume = VK_NULL_HANDLE;  // fs_clouds_volume
    VkShaderModule shape = VK_NULL_HANDLE;   // cs_shape
    VkShaderModule detail = VK_NULL_HANDLE;  // cs_detail
    VkShaderModule quantiles = VK_NULL_HANDLE;
};

class CloudsGpu {
public:
    // The pipeline draws into `colour` with `depth` to test against, by
    // `model`.
    CloudsGpu(const GpuContext& gpu, VkFormat colour, VkFormat depth, const CloudShaders& shaders,
              CloudModel model);
    ~CloudsGpu();
    CloudsGpu(const CloudsGpu&) = delete;
    CloudsGpu& operator=(const CloudsGpu&) = delete;

    // Before any view's clouds, every frame: the first makes the noise
    // (compute, then mips), the rest do nothing.
    void prepare(VkCommandBuffer cmd);

    // Draws one view's cloud layers inside a rendering pass on its resolved
    // target and depth, in `order` (indices into its lighting block's
    // layers, the farthest from the eye's height first). No allocation.
    void record(VkCommandBuffer cmd, const SceneView& view, std::uint32_t lighting_offset,
                std::span<const std::uint32_t> order);

private:
    struct Volume {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        VkImageView sampled = VK_NULL_HANDLE;  // every mip
        VkImageView storage = VK_NULL_HANDLE;  // mip 0
        std::uint32_t size = 0;
        std::uint32_t mips = 0;
    };
    Volume make_volume(std::uint32_t size);
    void make_mips(VkCommandBuffer cmd, const Volume& v);
    void release();

    GpuContext gpu_;
    Volume shape_;
    Volume detail_;
    VkBuffer table_ = VK_NULL_HANDLE;
    VmaAllocation table_memory_ = nullptr;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout noise_layout_ = VK_NULL_HANDLE;  // the compute passes'
    VkDescriptorSetLayout draw_layout_ = VK_NULL_HANDLE;   // the draw's set 1
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet noise_set_ = VK_NULL_HANDLE;
    VkDescriptorSet draw_set_ = VK_NULL_HANDLE;
    VkPipelineLayout compute_layout_ = VK_NULL_HANDLE;
    VkPipeline compute_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    bool made_ = false;
};

}  // namespace fovea
