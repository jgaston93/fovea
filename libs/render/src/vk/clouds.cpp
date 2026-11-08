#include "clouds.hpp"

#include <array>
#include <stdexcept>
#include <string>

namespace fovea {

namespace {

// Mirrors CloudPush in shaders/clouds.slang.
struct CloudPush {
    Mat4 clip_to_rel{1.0F};
    Mat4 rel_to_clip{1.0F};
};
static_assert(sizeof(CloudPush) == 128, "must match CloudPush in clouds.slang");

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what);
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fail(std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")");
    }
}

}  // namespace

CloudsGpu::CloudsGpu(const GpuContext& gpu, VkFormat colour, VkFormat depth, VkShaderModule vertex,
                     VkShaderModule fragment)
    : gpu_(gpu) {
    try {
        const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CloudPush)};
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &gpu_.frame_layout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(gpu_.device, &plci, nullptr, &layout_),
              "vkCreatePipelineLayout (clouds)");

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        for (VkPipelineShaderStageCreateInfo& st : stages) {
            st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st.pName = "main";
        }
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vertex;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fragment;
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
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
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        // Where nothing nearer than the cloud was drawn (reverse-Z: its depth
        // at least the scene's), which it doesn't write.
        VkPipelineDepthStencilStateCreateInfo depth_state{};
        depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_state.depthTestEnable = VK_TRUE;
        depth_state.depthWriteEnable = VK_FALSE;
        depth_state.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
        // Over what is behind it, premultiplied.
        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.blendEnable = VK_TRUE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;
        const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                           VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
        dynamic.pDynamicStates = dynamic_states.data();
        VkPipelineRenderingCreateInfo rendering{};
        rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colour;
        rendering.depthAttachmentFormat = depth;
        VkGraphicsPipelineCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        ci.pNext = &rendering;
        ci.stageCount = static_cast<std::uint32_t>(stages.size());
        ci.pStages = stages.data();
        ci.pVertexInputState = &vertex_input;
        ci.pInputAssemblyState = &assembly;
        ci.pViewportState = &viewport;
        ci.pRasterizationState = &raster;
        ci.pMultisampleState = &multisample;
        ci.pDepthStencilState = &depth_state;
        ci.pColorBlendState = &blend;
        ci.pDynamicState = &dynamic;
        ci.layout = layout_;
        check(vkCreateGraphicsPipelines(gpu_.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline_),
              "vkCreateGraphicsPipelines (clouds)");
    } catch (...) {
        release();
        throw;
    }
}

CloudsGpu::~CloudsGpu() {
    release();
}

void CloudsGpu::release() {
    if (gpu_.device == VK_NULL_HANDLE) {
        return;
    }
    vkDestroyPipeline(gpu_.device, pipeline_, nullptr);
    vkDestroyPipelineLayout(gpu_.device, layout_, nullptr);
    gpu_.device = VK_NULL_HANDLE;
}

void CloudsGpu::record(VkCommandBuffer cmd, const SceneView& view, std::uint32_t lighting_offset,
                       std::span<const std::uint32_t> order) {
    if (order.empty()) {
        return;
    }
    CloudPush push;
    push.clip_to_rel = Mat4(glm::inverse(DMat4(view.ecef_to_clip)));
    push.rel_to_clip = view.ecef_to_clip;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &gpu_.frame_set, 1,
                            &lighting_offset);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    // The layer is the instance (its index in the lighting block).
    for (const std::uint32_t layer : order) {
        vkCmdDraw(cmd, 3, 1, 0, layer);
    }
}

}  // namespace fovea
