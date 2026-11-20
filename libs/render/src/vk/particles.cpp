#include "particles.hpp"

#include <array>
#include <span>
#include <stdexcept>
#include <string>

namespace fovea {

namespace {

// The origins' part of each slot's buffer, then the particles'.
constexpr VkDeviceSize kOriginsBytes = sizeof(GpuOrigin) * kMaxParticleOrigins;
static_assert(kOriginsBytes % 256 == 0, "the particles' offset must suit any storage alignment");

// Mirrors ParticlePush in shaders/particles.slang.
struct ParticlePush {
    Mat4 ecef_to_clip{1.0F};
    std::array<float, 3> anchor{};  // the origins' anchor relative to the eye
    float forward_scattering = 0.0F;
    std::array<float, 3> albedo{};
    float unused = 0.0F;
};
static_assert(sizeof(ParticlePush) == 96, "must match ParticlePush in particles.slang");

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("particles: " + what);
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fail(std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")");
    }
}

// Triangles, single-sampled, no depth attachment (the shader reads the
// resolved depth), composited "over", premultiplied; the target's alpha is
// left as it was.
VkPipeline make_pipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule vertex,
                         VkShaderModule fragment, VkFormat colour) {
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
    VkPipelineDepthStencilStateCreateInfo depth_state{};
    depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
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
          "vkCreateGraphicsPipelines (particles)");
    return pipeline;
}

std::array<float, 3> floats3(const DVec3& v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

}  // namespace

ParticlesGpu::ParticlesGpu(const GpuContext& gpu, std::size_t capacity, std::uint32_t slots,
                           std::uint32_t outputs, VkFormat colour, VkShaderModule vertex,
                           VkShaderModule fragment)
    : gpu_(gpu),
      capacity_(capacity),
      slots_(slots),
      counts_(slots, 0),
      anchors_(slots, DVec3(0.0)),
      depth_sets_(outputs, VK_NULL_HANDLE) {
    if (capacity == 0 || slots == 0 || outputs == 0) {
        fail("no capacity, slots or outputs");
    }
    try {
        const VkDeviceSize particles_bytes = sizeof(GpuParticle) * capacity;
        for (Slot& s : slots_) {
            VkBufferCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = kOriginsBytes + particles_bytes;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            check(vmaCreateBuffer(gpu_.allocator, &bci, &aci, &s.buffer, &s.memory, &info),
                  "vmaCreateBuffer (particles)");
            if (info.pMappedData == nullptr) {
                fail("vmaCreateBuffer (particles): not mapped");
            }
            s.mapped = info.pMappedData;
        }

        const std::array<VkDescriptorSetLayoutBinding, 2> buffer_bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_VERTEX_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                         VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = static_cast<std::uint32_t>(buffer_bindings.size());
        lci.pBindings = buffer_bindings.data();
        check(vkCreateDescriptorSetLayout(gpu_.device, &lci, nullptr, &buffers_layout_),
              "vkCreateDescriptorSetLayout (particles)");
        const VkDescriptorSetLayoutBinding depth_binding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        lci.bindingCount = 1;
        lci.pBindings = &depth_binding;
        check(vkCreateDescriptorSetLayout(gpu_.device, &lci, nullptr, &depth_layout_),
              "vkCreateDescriptorSetLayout (particles' depth)");

        const std::array<VkDescriptorPoolSize, 2> sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * slots},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, outputs}};
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = slots + outputs;
        pci.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pci.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(gpu_.device, &pci, nullptr, &pool_),
              "vkCreateDescriptorPool (particles)");
        for (Slot& s : slots_) {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = pool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &buffers_layout_;
            check(vkAllocateDescriptorSets(gpu_.device, &ai, &s.set),
                  "vkAllocateDescriptorSets (particles)");
            const std::array<VkDescriptorBufferInfo, 2> infos{
                VkDescriptorBufferInfo{s.buffer, kOriginsBytes, particles_bytes},
                VkDescriptorBufferInfo{s.buffer, 0, kOriginsBytes}};
            std::array<VkWriteDescriptorSet, 2> writes{};
            for (std::uint32_t b = 0; b < 2; ++b) {
                writes.at(b).sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes.at(b).dstSet = s.set;
                writes.at(b).dstBinding = b;
                writes.at(b).descriptorCount = 1;
                writes.at(b).descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes.at(b).pBufferInfo = &infos.at(b);
            }
            vkUpdateDescriptorSets(gpu_.device, static_cast<std::uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
        for (VkDescriptorSet& d : depth_sets_) {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = pool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &depth_layout_;
            check(vkAllocateDescriptorSets(gpu_.device, &ai, &d),
                  "vkAllocateDescriptorSets (particles' depth)");
        }

        // Set 0 the buffers, set 1 the frame set (the view's lighting), set 2
        // the view's depth.
        const std::array<VkDescriptorSetLayout, 3> layouts{buffers_layout_, gpu_.frame_layout,
                                                           depth_layout_};
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(ParticlePush)};
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = static_cast<std::uint32_t>(layouts.size());
        plci.pSetLayouts = layouts.data();
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(gpu_.device, &plci, nullptr, &layout_),
              "vkCreatePipelineLayout (particles)");
        pipeline_ = make_pipeline(gpu_.device, layout_, vertex, fragment, colour);
    } catch (...) {
        release();
        throw;
    }
}

ParticlesGpu::~ParticlesGpu() {
    release();
}

void ParticlesGpu::release() {
    if (gpu_.device == VK_NULL_HANDLE) {
        return;
    }
    vkDestroyPipeline(gpu_.device, pipeline_, nullptr);
    vkDestroyPipelineLayout(gpu_.device, layout_, nullptr);
    vkDestroyDescriptorPool(gpu_.device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(gpu_.device, buffers_layout_, nullptr);
    vkDestroyDescriptorSetLayout(gpu_.device, depth_layout_, nullptr);
    for (Slot& s : slots_) {
        if (s.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(gpu_.allocator, s.buffer, s.memory);
        }
    }
    gpu_.device = VK_NULL_HANDLE;
}

void ParticlesGpu::set_depth(std::uint32_t output, VkImageView depth) {
    const VkDescriptorImageInfo info{VK_NULL_HANDLE, depth,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = depth_sets_.at(output);
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(gpu_.device, 1, &w, 0, nullptr);
}

std::size_t ParticlesGpu::pack(std::uint32_t slot, const ParticleFrame& frame, LocalTime at,
                               const ParticleLook& look, const DVec3& anchor) noexcept {
    Slot& s = slots_[slot];
    auto* base = static_cast<std::uint8_t*>(s.mapped);
    const std::span<GpuOrigin> origins(reinterpret_cast<GpuOrigin*>(base), kMaxParticleOrigins);
    const std::span<GpuParticle> particles(reinterpret_cast<GpuParticle*>(base + kOriginsBytes),
                                           capacity_);
    const std::size_t n = pack_particles(frame, at, look, anchor, particles, origins);
    (void)vmaFlushAllocation(gpu_.allocator, s.memory, 0, kOriginsBytes + sizeof(GpuParticle) * n);
    counts_[slot] = n;
    anchors_[slot] = anchor;
    return n;
}

void ParticlesGpu::record(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t output,
                          const SceneView& view, std::uint32_t lighting_offset,
                          const ParticleLook& look) const {
    const std::size_t n = counts_[slot];
    if (n == 0) {
        return;
    }
    ParticlePush push;
    push.ecef_to_clip = view.ecef_to_clip;
    push.anchor = floats3(anchors_[slot] - view.eye_ecef);
    push.forward_scattering = look.forward_scattering;
    push.albedo = look.albedo;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const std::array<VkDescriptorSet, 3> sets{slots_[slot].set, gpu_.frame_set,
                                              depth_sets_.at(output)};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0,
                            static_cast<std::uint32_t>(sets.size()), sets.data(), 1,
                            &lighting_offset);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDraw(cmd, static_cast<std::uint32_t>(n * 6), 1, 0, 0);
}

}  // namespace fovea
