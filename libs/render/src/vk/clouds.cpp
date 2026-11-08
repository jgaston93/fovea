#include "clouds.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>
#include <string>

namespace fovea {

namespace {

// The noise volumes' sizes (cloud_noise.slang's), and the table's: a
// histogram's 256 counts and 256 quantiles.
constexpr std::uint32_t kShapeSize = 128;
constexpr std::uint32_t kDetailSize = 32;
constexpr VkDeviceSize kTableBytes = 512 * sizeof(std::uint32_t);
constexpr VkFormat kNoiseFormat = VK_FORMAT_R8G8B8A8_UNORM;

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

CloudsGpu::CloudsGpu(const GpuContext& gpu, VkFormat colour, VkFormat depth,
                     const CloudShaders& shaders, CloudModel model)
    : gpu_(gpu) {
    try {
        shape_ = make_volume(kShapeSize);
        detail_ = make_volume(kDetailSize);
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = kTableBytes;
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        check(vmaCreateBuffer(gpu_.allocator, &bci, &aci, &table_, &table_memory_, nullptr),
              "vmaCreateBuffer (clouds)");
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        check(vkCreateSampler(gpu_.device, &sci, nullptr, &sampler_), "vkCreateSampler (clouds)");

        // The compute passes' set: the volumes' mip 0 to write, and the table.
        const std::array<VkDescriptorSetLayoutBinding, 3> noise_bindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        // The draw's: the volumes to sample, and the table.
        const std::array<VkDescriptorSetLayoutBinding, 3> draw_bindings = {
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 3;
        lci.pBindings = noise_bindings.data();
        check(vkCreateDescriptorSetLayout(gpu_.device, &lci, nullptr, &noise_layout_),
              "vkCreateDescriptorSetLayout (cloud noise)");
        lci.pBindings = draw_bindings.data();
        check(vkCreateDescriptorSetLayout(gpu_.device, &lci, nullptr, &draw_layout_),
              "vkCreateDescriptorSetLayout (clouds)");
        const std::array<VkDescriptorPoolSize, 3> sizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 2;
        pci.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pci.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(gpu_.device, &pci, nullptr, &pool_),
              "vkCreateDescriptorPool (clouds)");
        const std::array<VkDescriptorSetLayout, 2> set_layouts = {noise_layout_, draw_layout_};
        std::array<VkDescriptorSet, 2> sets{};
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 2;
        ai.pSetLayouts = set_layouts.data();
        check(vkAllocateDescriptorSets(gpu_.device, &ai, sets.data()),
              "vkAllocateDescriptorSets (clouds)");
        noise_set_ = sets[0];
        draw_set_ = sets[1];
        const VkDescriptorImageInfo shape_store{VK_NULL_HANDLE, shape_.storage,
                                                VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo detail_store{VK_NULL_HANDLE, detail_.storage,
                                                 VK_IMAGE_LAYOUT_GENERAL};
        const VkDescriptorImageInfo shape_read{sampler_, shape_.sampled,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorImageInfo detail_read{sampler_, detail_.sampled,
                                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const VkDescriptorBufferInfo table_info{table_, 0, VK_WHOLE_SIZE};
        std::array<VkWriteDescriptorSet, 6> ws{};
        for (std::uint32_t w = 0; w < ws.size(); ++w) {
            ws[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ws[w].dstSet = w < 3 ? noise_set_ : draw_set_;
            ws[w].dstBinding = w % 3;
            ws[w].descriptorCount = 1;
        }
        ws[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ws[0].pImageInfo = &shape_store;
        ws[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ws[1].pImageInfo = &detail_store;
        ws[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ws[2].pBufferInfo = &table_info;
        ws[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ws[3].pImageInfo = &shape_read;
        ws[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ws[4].pImageInfo = &detail_read;
        ws[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ws[5].pBufferInfo = &table_info;
        vkUpdateDescriptorSets(gpu_.device, static_cast<std::uint32_t>(ws.size()), ws.data(), 0,
                               nullptr);

        VkPipelineLayoutCreateInfo cplci{};
        cplci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        cplci.setLayoutCount = 1;
        cplci.pSetLayouts = &noise_layout_;
        check(vkCreatePipelineLayout(gpu_.device, &cplci, nullptr, &compute_layout_),
              "vkCreatePipelineLayout (cloud noise)");
        const std::array<VkShaderModule, 3> compute_modules = {shaders.shape, shaders.detail,
                                                               shaders.quantiles};
        for (std::size_t c = 0; c < compute_modules.size(); ++c) {
            VkComputePipelineCreateInfo cpci{};
            cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpci.stage.module = compute_modules[c];
            cpci.stage.pName = "main";
            cpci.layout = compute_layout_;
            check(vkCreateComputePipelines(gpu_.device, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                           &compute_[c]),
                  "vkCreateComputePipelines (cloud noise)");
        }

        const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CloudPush)};
        const std::array<VkDescriptorSetLayout, 2> draw_sets = {gpu_.frame_layout, draw_layout_};
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = static_cast<std::uint32_t>(draw_sets.size());
        plci.pSetLayouts = draw_sets.data();
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(gpu_.device, &plci, nullptr, &layout_),
              "vkCreatePipelineLayout (clouds)");
        const VkShaderModule vertex = shaders.vertex;
        const VkShaderModule fragment =
            model == CloudModel::volumetric ? shaders.volume : shaders.layer;

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
    for (VkPipeline c : compute_) {
        vkDestroyPipeline(gpu_.device, c, nullptr);
    }
    vkDestroyPipelineLayout(gpu_.device, compute_layout_, nullptr);
    vkDestroyDescriptorPool(gpu_.device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(gpu_.device, noise_layout_, nullptr);
    vkDestroyDescriptorSetLayout(gpu_.device, draw_layout_, nullptr);
    vkDestroySampler(gpu_.device, sampler_, nullptr);
    if (table_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(gpu_.allocator, table_, table_memory_);
    }
    for (Volume* v : {&shape_, &detail_}) {
        vkDestroyImageView(gpu_.device, v->sampled, nullptr);
        vkDestroyImageView(gpu_.device, v->storage, nullptr);
        if (v->image != VK_NULL_HANDLE) {
            vmaDestroyImage(gpu_.allocator, v->image, v->memory);
        }
    }
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
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 1, 1, &draw_set_, 0,
                            nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    // The layer is the instance (its index in the lighting block).
    for (const std::uint32_t layer : order) {
        vkCmdDraw(cmd, 3, 1, 0, layer);
    }
}

CloudsGpu::Volume CloudsGpu::make_volume(std::uint32_t size) {
    Volume v;
    v.size = size;
    v.mips = static_cast<std::uint32_t>(std::bit_width(size));
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_3D;
    ici.format = kNoiseFormat;
    ici.extent = {size, size, size};
    ici.mipLevels = v.mips;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    check(vmaCreateImage(gpu_.allocator, &ici, &aci, &v.image, &v.memory, nullptr),
          "vmaCreateImage (cloud noise)");
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = v.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vci.format = kNoiseFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, v.mips, 0, 1};
    check(vkCreateImageView(gpu_.device, &vci, nullptr, &v.sampled),
          "vkCreateImageView (cloud noise)");
    vci.subresourceRange.levelCount = 1;
    check(vkCreateImageView(gpu_.device, &vci, nullptr, &v.storage),
          "vkCreateImageView (cloud noise)");
    return v;
}

namespace {

void barrier(VkCommandBuffer cmd, VkImage image, std::uint32_t mip, std::uint32_t mips,
             VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src_stage,
             VkAccessFlags2 src, VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip, mips, 0, 1};
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace

// Mip 0 written (GENERAL) to every mip readable: each mip blitted from the
// one above, halving.
void CloudsGpu::make_mips(VkCommandBuffer cmd, const Volume& v) {
    barrier(cmd, v.image, 0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    for (std::uint32_t m = 1; m < v.mips; ++m) {
        barrier(cmd, v.image, m, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const auto from = static_cast<std::int32_t>(std::max(v.size >> (m - 1), 1U));
        const auto to = static_cast<std::int32_t>(std::max(v.size >> m, 1U));
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m - 1, 0, 1};
        blit.srcOffsets[1] = {from, from, from};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        blit.dstOffsets[1] = {to, to, to};
        vkCmdBlitImage(cmd, v.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, v.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
        barrier(cmd, v.image, m, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    barrier(cmd, v.image, 0, v.mips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_BLIT_BIT,
            VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

void CloudsGpu::prepare(VkCommandBuffer cmd) {
    if (made_) {
        return;
    }
    made_ = true;
    for (const Volume* v : {&shape_, &detail_}) {
        barrier(cmd, v->image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }
    // The histogram from zero.
    vkCmdFillBuffer(cmd, table_, 0, kTableBytes, 0);
    VkBufferMemoryBarrier2 bb{};
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
    bb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    bb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    bb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    bb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = table_;
    bb.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &bb;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_layout_, 0, 1, &noise_set_,
                            0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_[0]);
    vkCmdDispatch(cmd, kShapeSize / 4, kShapeSize / 4, kShapeSize / 4);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_[1]);
    vkCmdDispatch(cmd, kDetailSize / 4, kDetailSize / 4, kDetailSize / 4);
    // The quantiles, after the whole histogram.
    bb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    bb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compute_[2]);
    vkCmdDispatch(cmd, 1, 1, 1);
    bb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    bb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &dep);
    make_mips(cmd, shape_);
    make_mips(cmd, detail_);
}

}  // namespace fovea
