#include "shadows.hpp"

#include <array>
#include <stdexcept>
#include <string>

namespace fovea {

namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what);
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fail(std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")");
    }
}

}  // namespace

ShadowMaps::ShadowMaps(const GpuContext& gpu, std::uint32_t views)
    : gpu_(gpu), views_(std::max(views, 1U)) {
    try {
        const std::uint32_t layers = views_ * kShadowCascades;
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = kShadowFormat;
        ici.extent = {kShadowMapSize, kShadowMapSize, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = layers;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        check(vmaCreateImage(gpu_.allocator, &ici, &aci, &image_, &memory_, nullptr),
              "vmaCreateImage (shadow maps)");
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = image_;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        vci.format = kShadowFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, layers};
        check(vkCreateImageView(gpu_.device, &vci, nullptr, &array_view_),
              "vkCreateImageView (shadow maps)");
        layer_views_.assign(layers, VK_NULL_HANDLE);
        for (std::uint32_t l = 0; l < layers; ++l) {
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, l, 1};
            check(vkCreateImageView(gpu_.device, &vci, nullptr, &layer_views_[l]),
                  "vkCreateImageView (shadow map)");
        }
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        // Nearest, the comparison made in the shaders (frame.slang's
        // shadow_lit says why).
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;  // depth 1: lit
        check(vkCreateSampler(gpu_.device, &sci, nullptr, &sampler_),
              "vkCreateSampler (shadow maps)");
    } catch (...) {
        release();
        throw;
    }
}

ShadowMaps::~ShadowMaps() {
    release();
}

void ShadowMaps::release() {
    if (gpu_.device == VK_NULL_HANDLE) {
        return;
    }
    vkDestroySampler(gpu_.device, sampler_, nullptr);
    for (VkImageView v : layer_views_) {
        vkDestroyImageView(gpu_.device, v, nullptr);
    }
    vkDestroyImageView(gpu_.device, array_view_, nullptr);
    if (image_ != VK_NULL_HANDLE) {
        vmaDestroyImage(gpu_.allocator, image_, memory_);
    }
    gpu_.device = VK_NULL_HANDLE;
}

void ShadowMaps::begin(VkCommandBuffer cmd, std::uint32_t view, std::uint32_t cascade) const {
    const std::uint32_t layer = std::min(view, views_ - 1) * kShadowCascades + cascade;
    // After the frame before sampled it (its old depths are discarded).
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.srcAccessMask = 0;
    b.dstStageMask =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                      VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image_;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, layer, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);

    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = layer_views_[layer];
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.clearValue.depthStencil = {1.0F, 0};
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, {kShadowMapSize, kShadowMapSize}};
    info.layerCount = 1;
    info.pDepthAttachment = &depth;
    vkCmdBeginRendering(cmd, &info);
    const VkViewport viewport{
        0.0F, 0.0F, static_cast<float>(kShadowMapSize), static_cast<float>(kShadowMapSize),
        0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, {kShadowMapSize, kShadowMapSize}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void ShadowMaps::done(VkCommandBuffer cmd, std::uint32_t view) const {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image_;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1,
                          std::min(view, views_ - 1) * kShadowCascades, kShadowCascades};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

VkPipeline make_shadow_pipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule vertex,
                                VkShaderModule fragment,
                                const VkPipelineVertexInputStateCreateInfo* vertex_input) {
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    for (VkPipelineShaderStageCreateInfo& s : stages) {
        s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.pName = "main";
    }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    VkPipelineVertexInputStateCreateInfo no_input{};
    no_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0F;
    // No depth bias: a slope-scaled one pushes a post's sides, which a low
    // sun grazes, a long way back and shortens its shadow by metres. The
    // receivers offset themselves along their normals instead (shadow_lit).
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_state{};
    depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_state.depthTestEnable = VK_TRUE;
    depth_state.depthWriteEnable = VK_TRUE;
    depth_state.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;  // 0 is nearest the light
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                       VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();
    const VkFormat depth_format = kShadowFormat;
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.depthAttachmentFormat = depth_format;
    VkGraphicsPipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    ci.pNext = &rendering;
    ci.stageCount = fragment != VK_NULL_HANDLE ? 2U : 1U;
    ci.pStages = stages.data();
    ci.pVertexInputState = vertex_input != nullptr ? vertex_input : &no_input;
    ci.pInputAssemblyState = &assembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = &depth_state;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dynamic;
    ci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline),
          "vkCreateGraphicsPipelines (shadows)");
    return pipeline;
}

}  // namespace fovea
