#include "stars.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

#include "fovea/core/geodesy.hpp"

namespace fovea {

namespace {

// {right ascension, declination (J2000, degrees), V magnitude, B-V}.
struct CatalogStar {
    float ra_deg, dec_deg, vmag, bv;
};
constexpr auto kCatalog = std::to_array<CatalogStar>({
#include "star_catalog.inc"
});

// Mirrors StarPush in shaders/stars.slang.
struct StarPush {
    Mat4 j2000_to_clip{1.0F};
    std::array<float, 3> up{};
    float inv_pixel_solid_angle = 0.0F;
};
static_assert(sizeof(StarPush) == 80, "must match StarPush in stars.slang");

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what);
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fail(std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")");
    }
}

}  // namespace

// The solid angle of a pixel on a view's axis, from its matrix (off it, the
// shader takes cos^3 of the angle):
// clip x is sx times the view's sideways direction plus an off-centre
// share of its forward one, and clip w is the forward one alone.
double pixel_solid_angle(const Mat4& ecef_to_clip, VkExtent2D extent) {
    const auto row = [&](int r) {
        return DVec3(ecef_to_clip[0][r], ecef_to_clip[1][r], ecef_to_clip[2][r]);
    };
    const DVec3 w = row(3);
    const auto scale = [&](const DVec3& r) {
        const double along = glm::dot(r, w) / std::max(glm::dot(w, w), 1e-30);
        return glm::length(r - along * w) / std::max(glm::length(w), 1e-30);
    };
    // NDC spans 2 across the image: a pixel is 2 / (scale * size) radians.
    return (2.0 / (scale(row(0)) * extent.width)) * (2.0 / (scale(row(1)) * extent.height));
}

// A pipeline drawing points, single-sampled, added to `colour` where
// they pass a reverse-Z test against `depth`, which they don't write: the
// stars' and the lights'. `what` names it in errors.
VkPipeline make_point_pipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule vertex,
                               VkShaderModule fragment, VkFormat colour, VkFormat depth,
                               const char* what) {
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
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
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
    // Where nothing nearer was drawn (reverse-Z: greater is nearer);
    // stars, at infinity, pass only where the depth is still the sky's.
    VkPipelineDepthStencilStateCreateInfo depth_state{};
    depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_state.depthTestEnable = VK_TRUE;
    depth_state.depthWriteEnable = VK_FALSE;
    depth_state.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;
    // Added to the light drawn already.
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.blendEnable = VK_TRUE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
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
    ci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline),
          (std::string("vkCreateGraphicsPipelines (") + what + ")").c_str());
    return pipeline;
}

StarsGpu::StarsGpu(const GpuContext& gpu, VkFormat colour, VkFormat depth, VkShaderModule vertex,
                   VkShaderModule fragment)
    : gpu_(gpu), count_(static_cast<std::uint32_t>(kCatalog.size())) {
    try {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = sizeof(CatalogStar) * kCatalog.size();
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        check(vmaCreateBuffer(gpu_.allocator, &bci, &aci, &stars_, &stars_memory_, &info),
              "vmaCreateBuffer (stars)");
        if (info.pMappedData == nullptr) {
            fail("vmaCreateBuffer (stars): not mapped");
        }
        std::memcpy(info.pMappedData, kCatalog.data(), sizeof(CatalogStar) * kCatalog.size());
        (void)vmaFlushAllocation(gpu_.allocator, stars_memory_, 0, VK_WHOLE_SIZE);

        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                   VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 1;
        lci.pBindings = &binding;
        check(vkCreateDescriptorSetLayout(gpu_.device, &lci, nullptr, &set_layout_),
              "vkCreateDescriptorSetLayout (stars)");
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &size;
        check(vkCreateDescriptorPool(gpu_.device, &pci, nullptr, &pool_),
              "vkCreateDescriptorPool (stars)");
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        check(vkAllocateDescriptorSets(gpu_.device, &ai, &set_),
              "vkAllocateDescriptorSets (stars)");
        const VkDescriptorBufferInfo buffer_info{stars_, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set_;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(gpu_.device, 1, &w, 0, nullptr);

        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(StarPush)};
        const std::array<VkDescriptorSetLayout, 2> sets = {set_layout_, gpu_.frame_layout};
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = static_cast<std::uint32_t>(sets.size());
        plci.pSetLayouts = sets.data();
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(gpu_.device, &plci, nullptr, &layout_),
              "vkCreatePipelineLayout (stars)");

        pipeline_ =
            make_point_pipeline(gpu_.device, layout_, vertex, fragment, colour, depth, "stars");
    } catch (...) {
        release();
        throw;
    }
}

StarsGpu::~StarsGpu() {
    release();
}

void StarsGpu::release() {
    if (gpu_.device == VK_NULL_HANDLE) {
        return;
    }
    vkDestroyPipeline(gpu_.device, pipeline_, nullptr);
    vkDestroyPipelineLayout(gpu_.device, layout_, nullptr);
    vkDestroyDescriptorPool(gpu_.device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(gpu_.device, set_layout_, nullptr);
    if (stars_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(gpu_.allocator, stars_, stars_memory_);
    }
    gpu_.device = VK_NULL_HANDLE;
}

void StarsGpu::record(VkCommandBuffer cmd, const SceneView& view, const SkyBodies& sky,
                      VkExtent2D extent, std::uint32_t lighting_offset) {
    const Geodetic eye = ecef_to_geodetic(view.eye_ecef);
    const DVec3 up_ecef = ned_to_ecef(eye.lat_deg, eye.lon_deg) * DVec3(0.0, 0.0, -1.0);
    const DVec3 up = glm::transpose(sky.j2000_to_ecef) * up_ecef;
    // Directions only: the 3x3 rotation, then the view's matrix.
    const Mat4 rotation = Mat4(glm::mat3(sky.j2000_to_ecef));
    StarPush push;
    push.j2000_to_clip = view.ecef_to_clip * rotation;
    push.up = {static_cast<float>(up.x), static_cast<float>(up.y), static_cast<float>(up.z)};
    push.inv_pixel_solid_angle =
        static_cast<float>(1.0 / pixel_solid_angle(view.ecef_to_clip, extent));
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const std::array<VkDescriptorSet, 2> sets = {set_, gpu_.frame_set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0,
                            static_cast<std::uint32_t>(sets.size()), sets.data(), 1,
                            &lighting_offset);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(cmd, count_, 1, 0, 0);
}

}  // namespace fovea
