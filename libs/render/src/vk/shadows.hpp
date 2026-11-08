#pragma once

// The shadow maps (milestone 6 step 5, D-032): kShadowCascades layers of
// kShadowMapSize squared 32-bit depth a view, drawn each frame before the
// view's scene, from the light, by the terrain's and the models' shadow
// pipelines; then sampled, compared, by the scene's shaders.

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstdint>
#include <vector>

#include "fovea/render/shadow_cascades.hpp"
#include "gpu_context.hpp"

namespace fovea {

inline constexpr VkFormat kShadowFormat = VK_FORMAT_D32_SFLOAT;

class ShadowMaps {
public:
    ShadowMaps(const GpuContext& gpu, std::uint32_t views);
    ~ShadowMaps();
    ShadowMaps(const ShadowMaps&) = delete;
    ShadowMaps& operator=(const ShadowMaps&) = delete;

    // For the frame set and the terrain's set: every layer, and a nearest
    // sampler (outside the map, depth 1: lit).
    VkImageView maps() const { return array_view_; }
    VkSampler sampler() const { return sampler_; }

    // A view's cascade c: begins a depth-only rendering pass into it,
    // cleared to 1 (the far side), after the frame before read it. End it
    // with vkCmdEndRendering, then call done() once all of the view's are
    // drawn.
    void begin(VkCommandBuffer cmd, std::uint32_t view, std::uint32_t cascade) const;
    // The view's cascades, made readable to the scene's fragment shaders.
    void done(VkCommandBuffer cmd, std::uint32_t view) const;

private:
    void release();

    GpuContext gpu_;
    std::uint32_t views_ = 0;
    VkImage image_ = VK_NULL_HANDLE;
    VmaAllocation memory_ = nullptr;
    VkImageView array_view_ = VK_NULL_HANDLE;
    std::vector<VkImageView> layer_views_;
    VkSampler sampler_ = VK_NULL_HANDLE;
};

// A depth-only pipeline for the shadow maps: `fragment` may be null (no
// cut-outs); `vertex_input` null for none. Slope-scaled depth bias keeps a
// surface lit at a grazing light.
VkPipeline make_shadow_pipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule vertex,
                                VkShaderModule fragment,
                                const VkPipelineVertexInputStateCreateInfo* vertex_input);

}  // namespace fovea
