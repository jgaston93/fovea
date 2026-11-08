#include "models.hpp"

#include "shadows.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <stdexcept>

#include "fovea/core/geodesy.hpp"
#include "fovea/render/terrain_lod.hpp"
#include "fovea/terrain/database.hpp"

namespace fovea {

namespace {

// shaders/model.slang's blocks.
struct ModelDraw {
    std::array<std::array<float, 4>, 3> rows{};  // its frame to ECEF axes, relative to the eye
    std::uint32_t material = 0;
    std::array<std::uint32_t, 3> unused{};
};
static_assert(sizeof(ModelDraw) == 64, "must match ModelDraw in model.slang");

struct GpuMaterial {
    std::array<float, 4> base_colour{};
    std::uint32_t image = 0;  // kNoImage: none
    std::uint32_t sampler = 0;
    std::uint32_t alpha_mode = 0;
    float alpha_cutoff = 0.5F;
};
static_assert(sizeof(GpuMaterial) == 32, "must match Material in model.slang");

struct ModelPush {
    Mat4 ecef_to_clip{1.0F};
    std::uint32_t samples = 1;  // the scene pass's: masks use alpha to coverage past 1
};
static_assert(sizeof(ModelPush) == 68, "must match ModelPush in model.slang");

constexpr std::uint32_t kNoImage = 0xFFFFFFFFU;
static_assert(sizeof(ModelVertex) == 32, "the vertex layout below");

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what);
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fail(std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")");
    }
}

VkDeviceSize align16(VkDeviceSize n) {
    return (n + 15U) & ~VkDeviceSize{15};
}

VkSamplerAddressMode address(std::uint32_t wrap) {
    switch (static_cast<TextureWrap>(wrap)) {
        case TextureWrap::clamp:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case TextureWrap::mirror:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case TextureWrap::repeat:
            break;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

void barrier(VkCommandBuffer cmd, const VkImageMemoryBarrier2* image, const VkMemoryBarrier2* mem) {
    VkDependencyInfo d{};
    d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    d.imageMemoryBarrierCount = image != nullptr ? 1 : 0;
    d.pImageMemoryBarriers = image;
    d.memoryBarrierCount = mem != nullptr ? 1 : 0;
    d.pMemoryBarriers = mem;
    vkCmdPipelineBarrier2(cmd, &d);
}

// A placement's frame: glTF's +Y up along the ellipsoid normal, +Z its
// front along the heading (clockwise from true north), +X to its left.
DMat3 placement_rotation(const Placement& p) {
    const DMat3 ned = ned_to_ecef(p.lat_deg, p.lon_deg);
    const DVec3 north = ned[0];
    const DVec3 east = ned[1];
    const DVec3 up = -ned[2];
    const double h = glm::radians(static_cast<double>(p.heading_deg));
    const DVec3 front = north * std::cos(h) + east * std::sin(h);
    return DMat3(glm::cross(up, front), up, front);
}

}  // namespace

ModelGpu::Buffer ModelGpu::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                       bool host) const {
    Buffer b;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (host) {
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VmaAllocationInfo info{};
    check(vmaCreateBuffer(gpu_.allocator, &bci, &aci, &b.buffer, &b.memory, &info),
          "vmaCreateBuffer (models)");
    b.mapped = info.pMappedData;
    return b;
}

void ModelGpu::destroy(Buffer& b) const {
    if (b.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(gpu_.allocator, b.buffer, b.memory);
    }
    b = {};
}

void ModelGpu::destroy(Image& i) const {
    if (i.view != VK_NULL_HANDLE) {
        vkDestroyImageView(gpu_.device, i.view, nullptr);
    }
    if (i.image != VK_NULL_HANDLE) {
        vmaDestroyImage(gpu_.allocator, i.image, i.memory);
    }
    i = {};
}

ModelGpu::ModelGpu(const GpuContext& gpu, std::filesystem::path database_dir,
                   std::span<const Placement> placements, std::uint32_t slots,
                   std::uint32_t outputs, const ModelShaders& shaders, VkFormat colour,
                   VkFormat depth)
    : gpu_(gpu), dir_(std::move(database_dir)), outputs_(std::max(outputs, 1U)) {
    if (placements.size() > kMaxPlacements) {
        fail("models: " + std::to_string(placements.size()) + " placements, more than " +
             std::to_string(kMaxPlacements));
    }
    // Each distinct model once, in name order.
    std::map<std::string, std::uint32_t> index;
    for (const Placement& p : placements) {
        index.emplace(p.model, 0U);
    }
    for (auto& [name, i] : index) {
        i = static_cast<std::uint32_t>(models_.size());
        models_.push_back({});
        models_.back().name = name;
    }
    placed_.reserve(placements.size());
    for (const Placement& p : placements) {
        placed_.push_back({index.at(p.model), geodetic_to_ecef({p.lat_deg, p.lon_deg, p.height_m}),
                           placement_rotation(p)});
    }
    images_.resize(kMaxModelImages);
    VkDeviceSize files = 0;
    for (const Entry& e : models_) {
        std::error_code ec;
        const auto size = valid_model_name(e.name)
                              ? std::filesystem::file_size(dir_ / model_path(e.name), ec)
                              : 0;
        files += ec ? 0 : size;
    }
    vertex_capacity_ = std::clamp(files * 2, kMinModelBufferBytes, kMaxModelBufferBytes);
    index_capacity_ = vertex_capacity_;

    try {
        vertices_ = make_buffer(
            vertex_capacity_, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            false);
        indices_ =
            make_buffer(index_capacity_,
                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        materials_ = make_buffer(
            VkDeviceSize{kMaxModelMaterials} * sizeof(GpuMaterial),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        const VkDeviceSize per = VkDeviceSize{slots} * outputs_ * kRegions * kMaxModelDraws;
        draws_ = make_buffer(per * sizeof(ModelDraw), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        commands_ = make_buffer(per * sizeof(VkDrawIndexedIndirectCommand),
                                VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);

        for (std::uint32_t u = 0; u < 3; ++u) {
            for (std::uint32_t v = 0; v < 3; ++v) {
                VkSamplerCreateInfo sci{};
                sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
                sci.magFilter = VK_FILTER_LINEAR;
                sci.minFilter = VK_FILTER_LINEAR;
                sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                sci.addressModeU = address(u);
                sci.addressModeV = address(v);
                sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
                sci.maxLod = VK_LOD_CLAMP_NONE;
                if (gpu.max_anisotropy > 1.0F) {
                    sci.anisotropyEnable = VK_TRUE;
                    sci.maxAnisotropy = std::min(gpu.max_anisotropy, 16.0F);
                }
                check(vkCreateSampler(gpu.device, &sci, nullptr, &samplers_[u * 3 + v]),
                      "vkCreateSampler (models)");
            }
        }

        std::array<VkDescriptorSetLayoutBinding, 4> bindings{};
        bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT,
                       nullptr};
        bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                       nullptr};
        bindings[2] = {2, VK_DESCRIPTOR_TYPE_SAMPLER, static_cast<std::uint32_t>(samplers_.size()),
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        bindings[3] = {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxModelImages,
                       VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        // Images arrive while frames using the set are in flight.
        const std::array<VkDescriptorBindingFlags, 4> flags = {
            0, 0, 0,
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT};
        VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{};
        flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
        flags_info.bindingCount = static_cast<std::uint32_t>(flags.size());
        flags_info.pBindingFlags = flags.data();
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.pNext = &flags_info;
        lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        lci.bindingCount = static_cast<std::uint32_t>(bindings.size());
        lci.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(gpu.device, &lci, nullptr, &set_layout_),
              "vkCreateDescriptorSetLayout (models)");
        const std::array<VkDescriptorPoolSize, 3> sizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER,
                                 static_cast<std::uint32_t>(samplers_.size())},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxModelImages}};
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        pci.maxSets = 1;
        pci.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pci.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(gpu.device, &pci, nullptr, &pool_),
              "vkCreateDescriptorPool (models)");
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        check(vkAllocateDescriptorSets(gpu.device, &ai, &set_),
              "vkAllocateDescriptorSets (models)");
        const VkDescriptorBufferInfo draws_info{draws_.buffer, 0, VK_WHOLE_SIZE};
        const VkDescriptorBufferInfo materials_info{materials_.buffer, 0, VK_WHOLE_SIZE};
        std::array<VkDescriptorImageInfo, 9> sampler_info{};
        for (std::size_t i = 0; i < samplers_.size(); ++i) {
            sampler_info[i] = {samplers_[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        }
        std::array<VkWriteDescriptorSet, 3> writes{};
        for (std::uint32_t w = 0; w < writes.size(); ++w) {
            writes[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[w].dstSet = set_;
            writes[w].dstBinding = w;
            writes[w].descriptorCount = bindings[w].descriptorCount;
            writes[w].descriptorType = bindings[w].descriptorType;
        }
        writes[0].pBufferInfo = &draws_info;
        writes[1].pBufferInfo = &materials_info;
        writes[2].pImageInfo = sampler_info.data();
        vkUpdateDescriptorSets(gpu.device, static_cast<std::uint32_t>(writes.size()), writes.data(),
                               0, nullptr);

        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 sizeof(ModelPush)};
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        const std::array<VkDescriptorSetLayout, 2> set_layouts = {set_layout_, gpu.frame_layout};
        plci.setLayoutCount = static_cast<std::uint32_t>(set_layouts.size());
        plci.pSetLayouts = set_layouts.data();
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(gpu.device, &plci, nullptr, &layout_),
              "vkCreatePipelineLayout (models)");
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = shaders.vertex;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = shaders.fragment;
        stages[1].pName = "main";
        const VkVertexInputBindingDescription binding{0, sizeof(ModelVertex),
                                                      VK_VERTEX_INPUT_RATE_VERTEX};
        const std::array<VkVertexInputAttributeDescription, 3> attributes = {
            VkVertexInputAttributeDescription{0, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(ModelVertex, position)},
            VkVertexInputAttributeDescription{1, 0, VK_FORMAT_R32G32B32_SFLOAT,
                                              offsetof(ModelVertex, normal)},
            VkVertexInputAttributeDescription{2, 0, VK_FORMAT_R32G32_SFLOAT,
                                              offsetof(ModelVertex, uv)}};
        VkPipelineVertexInputStateCreateInfo vertex_input{};
        vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();
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
        // Both sides drawn (the fragment shader turns a back face's normal
        // round): massing models are closed, and double-sided materials
        // need no second pipeline.
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0F;
        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = gpu.samples;
        // Masked materials' cut-outs smoothed (milestone 5 step 2): the
        // fragment's alpha covers that share of the pixel's samples. Every
        // other material writes alpha 1, all of them.
        multisample.alphaToCoverageEnable =
            gpu.samples != VK_SAMPLE_COUNT_1_BIT ? VK_TRUE : VK_FALSE;
        VkPipelineDepthStencilStateCreateInfo depth_state{};
        depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depth_state.depthTestEnable = VK_TRUE;
        depth_state.depthWriteEnable = VK_TRUE;
        depth_state.depthCompareOp = VK_COMPARE_OP_GREATER;  // reverse-Z
        VkPipelineColorBlendAttachmentState blend_attachment{};
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
        check(vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline_),
              "vkCreateGraphicsPipelines (models)");
        // The shadow maps' (step 5): the same vertices, depth only.
        // Without the normals: casting doesn't need them.
        const std::array<VkVertexInputAttributeDescription, 2> shadow_attributes = {attributes[0],
                                                                                    attributes[2]};
        VkPipelineVertexInputStateCreateInfo shadow_input = vertex_input;
        shadow_input.vertexAttributeDescriptionCount =
            static_cast<std::uint32_t>(shadow_attributes.size());
        shadow_input.pVertexAttributeDescriptions = shadow_attributes.data();
        shadow_pipeline_ = make_shadow_pipeline(gpu.device, layout_, shaders.shadow_vertex,
                                                shaders.shadow_fragment, &shadow_input);
    } catch (...) {
        release();
        throw;
    }
}

ModelGpu::~ModelGpu() {
    release();
}

void ModelGpu::release() {
    if (gpu_.device == VK_NULL_HANDLE) {
        return;
    }
    vkDestroyPipeline(gpu_.device, pipeline_, nullptr);
    vkDestroyPipeline(gpu_.device, shadow_pipeline_, nullptr);
    vkDestroyPipelineLayout(gpu_.device, layout_, nullptr);
    vkDestroyDescriptorPool(gpu_.device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(gpu_.device, set_layout_, nullptr);
    for (VkSampler s : samplers_) {
        vkDestroySampler(gpu_.device, s, nullptr);
    }
    for (Image& i : images_) {
        destroy(i);
    }
    destroy(vertices_);
    destroy(indices_);
    destroy(materials_);
    destroy(draws_);
    destroy(commands_);
    pipeline_ = VK_NULL_HANDLE;
    layout_ = VK_NULL_HANDLE;
    pool_ = VK_NULL_HANDLE;
    set_layout_ = VK_NULL_HANDLE;
    samplers_ = {};
    gpu_.device = VK_NULL_HANDLE;
}

std::optional<Model> ModelGpu::prepare(std::uint32_t m, std::string& problem) {
    const std::string& name = models_[m].name;
    if (!valid_model_name(name)) {
        problem = "model '" + name + "': not a valid name";
        return std::nullopt;
    }
    ModelResult r = read_model_file(dir_ / model_path(name));
    if (!r.model) {
        problem = "model '" + name + "': " + r.error;
        return std::nullopt;
    }
    const Model& model = *r.model;
    // Room in the shared buffers (never freed: a database places a fixed
    // set of models).
    if (vertex_used_ + model.vertices.size() * sizeof(ModelVertex) > vertex_capacity_ ||
        index_used_ + model.indices.size() * sizeof(std::uint32_t) > index_capacity_ ||
        materials_used_ + model.materials.size() > kMaxModelMaterials ||
        images_used_ + model.images.size() > kMaxModelImages) {
        problem = "model '" + name + "': no room left for it on the GPU (vertices " +
                  std::to_string(model.vertices.size()) + ", indices " +
                  std::to_string(model.indices.size()) + ", materials " +
                  std::to_string(model.materials.size()) + ", images " +
                  std::to_string(model.images.size()) + ")";
        return std::nullopt;
    }
    return std::move(r.model);
}

VkDeviceSize ModelGpu::staged_bytes(const Model& m) {
    VkDeviceSize bytes = align16(m.vertices.size() * sizeof(ModelVertex)) +
                         align16(m.indices.size() * sizeof(std::uint32_t)) +
                         align16(m.materials.size() * sizeof(GpuMaterial));
    for (const ModelImage& im : m.images) {
        for (const TextureLevel& l : im.levels) {
            bytes += align16(l.pixels.size());
        }
    }
    return bytes;
}

void ModelGpu::stage_and_record(VkCommandBuffer cmd, VkBuffer staging, std::uint8_t* mapped,
                                VkDeviceSize at, std::uint32_t mi, const Model& m) {
    Entry& e = models_[mi];
    const auto put = [&](const void* data, std::size_t bytes) {
        const VkDeviceSize here = at;
        if (bytes > 0) {
            std::memcpy(mapped + here, data, bytes);
        }
        at += align16(bytes);
        return here;
    };
    const auto copy_buffer = [&](const void* data, std::size_t bytes, VkBuffer dst,
                                 VkDeviceSize dst_at) {
        if (bytes == 0) {
            return;
        }
        VkBufferCopy c{};
        c.srcOffset = put(data, bytes);
        c.dstOffset = dst_at;
        c.size = bytes;
        vkCmdCopyBuffer(cmd, staging, dst, 1, &c);
    };

    // Images first: their slots name them in the materials.
    const std::uint32_t first_image = images_used_;
    e.images.clear();
    for (const ModelImage& im : m.images) {
        const std::uint32_t slot = images_used_++;
        const auto mips = static_cast<std::uint32_t>(im.levels.size());
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = im.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        ici.extent = {im.levels[0].width, im.levels[0].height, 1};
        ici.mipLevels = mips;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        Image& img = images_[slot];
        check(vmaCreateImage(gpu_.allocator, &ici, &aci, &img.image, &img.memory, nullptr),
              "vmaCreateImage (models)");
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = img.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = ici.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
        check(vkCreateImageView(gpu_.device, &vci, nullptr, &img.view),
              "vkCreateImageView (models)");
        VkImageMemoryBarrier2 b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = vci.subresourceRange;
        barrier(cmd, &b, nullptr);
        for (std::uint32_t l = 0; l < mips; ++l) {
            const TextureLevel& level = im.levels[l];
            VkBufferImageCopy copy{};
            copy.bufferOffset = put(level.pixels.data(), level.pixels.size());
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
            copy.imageExtent = {level.width, level.height, 1};
            vkCmdCopyBufferToImage(cmd, staging, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &copy);
        }
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier(cmd, &b, nullptr);
        e.images.push_back(slot);
    }

    std::vector<GpuMaterial> mats(m.materials.size());
    for (std::size_t i = 0; i < mats.size(); ++i) {
        const ModelMaterial& src = m.materials[i];
        GpuMaterial& g = mats[i];
        g.base_colour = src.base_colour;
        g.image = kNoImage;
        if (src.base_colour_texture) {
            const ModelTexture& t = m.textures[*src.base_colour_texture];
            g.image = first_image + t.image;
            g.sampler =
                static_cast<std::uint32_t>(t.wrap_u) * 3 + static_cast<std::uint32_t>(t.wrap_v);
        }
        g.alpha_mode = static_cast<std::uint32_t>(src.alpha_mode);
        g.alpha_cutoff = src.alpha_cutoff;
    }
    // Earlier frames may be drawing models already in these buffers: the
    // shaders' reads (the material buffer is bound whole) come before the
    // copies' writes.
    VkMemoryBarrier2 before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    before.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
                          VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier(cmd, nullptr, &before);
    const std::uint32_t first_material = materials_used_;
    const auto first_index = static_cast<std::uint32_t>(index_used_ / sizeof(std::uint32_t));
    e.vertex_offset = static_cast<std::int32_t>(vertex_used_ / sizeof(ModelVertex));
    copy_buffer(m.vertices.data(), m.vertices.size() * sizeof(ModelVertex), vertices_.buffer,
                vertex_used_);
    copy_buffer(m.indices.data(), m.indices.size() * sizeof(std::uint32_t), indices_.buffer,
                index_used_);
    copy_buffer(mats.data(), mats.size() * sizeof(GpuMaterial), materials_.buffer,
                VkDeviceSize{first_material} * sizeof(GpuMaterial));
    vertex_used_ += m.vertices.size() * sizeof(ModelVertex);
    index_used_ += m.indices.size() * sizeof(std::uint32_t);
    materials_used_ += static_cast<std::uint32_t>(mats.size());
    // After the copies, they're read as vertices, indices and storage.
    VkMemoryBarrier2 after{};
    after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    after.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT |
                         VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT |
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    after.dstAccessMask = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT |
                          VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    barrier(cmd, nullptr, &after);

    e.primitives.clear();
    for (const ModelPrimitive& prim : m.primitives) {
        e.primitives.push_back(
            {first_index + prim.first_index, prim.index_count, first_material + prim.material});
    }
    const DVec3 lo(m.min[0], m.min[1], m.min[2]);
    const DVec3 hi(m.max[0], m.max[1], m.max[2]);
    e.centre = (lo + hi) * 0.5;
    e.radius_m = glm::length(hi - lo) * 0.5 + 0.01;  // and float rounding
}

void ModelGpu::arrived(std::uint32_t m) {
    Entry& e = models_[m];
    for (const std::uint32_t slot : e.images) {
        const VkDescriptorImageInfo info{VK_NULL_HANDLE, images_[slot].view,
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set_;
        w.dstBinding = 3;
        w.dstArrayElement = slot;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        w.pImageInfo = &info;
        vkUpdateDescriptorSets(gpu_.device, 1, &w, 0, nullptr);
    }
    e.ready = true;
    ++loaded_;
}

std::uint32_t ModelGpu::record(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t output,
                               const SceneView& view, std::uint32_t lighting_offset,
                               std::uint32_t& dropped) {
    if (loaded_ == 0) {
        return 0;
    }
    const std::uint32_t o = std::min(output, outputs_ - 1);
    const std::uint32_t base = (slot * outputs_ + o) * kRegions * kMaxModelDraws;
    auto* records = static_cast<ModelDraw*>(draws_.mapped) + base;
    auto* commands = static_cast<VkDrawIndexedIndirectCommand*>(commands_.mapped) + base;
    std::uint32_t n = 0;
    std::uint32_t placements = 0;
    for (const Placed& p : placed_) {
        const Entry& e = models_[p.model];
        if (!e.ready) {
            continue;
        }
        const DVec3 offset = p.origin_ecef - view.eye_ecef;
        if (!sphere_visible(view.ecef_to_clip, offset + p.rotation * e.centre, e.radius_m)) {
            continue;
        }
        if (n + e.primitives.size() > kMaxModelDraws) {
            ++dropped;
            continue;
        }
        ModelDraw d;
        for (int r = 0; r < 3; ++r) {
            d.rows[static_cast<std::size_t>(r)] = {
                static_cast<float>(p.rotation[0][r]), static_cast<float>(p.rotation[1][r]),
                static_cast<float>(p.rotation[2][r]), static_cast<float>(offset[r])};
        }
        for (const Primitive& prim : e.primitives) {
            d.material = prim.material;
            records[n] = d;
            commands[n] = {prim.index_count, 1, prim.first_index, e.vertex_offset, base + n};
            ++n;
        }
        ++placements;
    }
    if (n == 0) {
        return 0;
    }
    (void)vmaFlushAllocation(gpu_.allocator, draws_.memory, base * sizeof(ModelDraw),
                             n * sizeof(ModelDraw));
    (void)vmaFlushAllocation(gpu_.allocator, commands_.memory,
                             base * sizeof(VkDrawIndexedIndirectCommand),
                             n * sizeof(VkDrawIndexedIndirectCommand));
    ModelPush push;
    push.ecef_to_clip = view.ecef_to_clip;
    push.samples = static_cast<std::uint32_t>(gpu_.samples);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    const std::array<VkDescriptorSet, 2> sets = {set_, gpu_.frame_set};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0,
                            static_cast<std::uint32_t>(sets.size()), sets.data(), 1,
                            &lighting_offset);
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDrawIndexedIndirect(cmd, commands_.buffer, base * sizeof(VkDrawIndexedIndirectCommand), n,
                             sizeof(VkDrawIndexedIndirectCommand));
    return placements;
}

void ModelGpu::record_shadow(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t output,
                             std::uint32_t cascade, const SceneView& view, const Mat4& to_map,
                             std::uint32_t& dropped) {
    if (loaded_ == 0) {
        return;
    }
    const std::uint32_t o = std::min(output, outputs_ - 1);
    const std::uint32_t base =
        ((slot * outputs_ + o) * kRegions + 1 + std::min(cascade, kShadowCascades - 1)) *
        kMaxModelDraws;
    auto* records = static_cast<ModelDraw*>(draws_.mapped) + base;
    auto* commands = static_cast<VkDrawIndexedIndirectCommand*>(commands_.mapped) + base;
    // The map's box: x and y -1 to 1, z 0 to 1; a sphere's reach in each.
    const DMat4 m(to_map);
    const double sx = glm::length(DVec3(m[0][0], m[1][0], m[2][0]));
    const double sy = glm::length(DVec3(m[0][1], m[1][1], m[2][1]));
    const double sz = glm::length(DVec3(m[0][2], m[1][2], m[2][2]));
    std::uint32_t n = 0;
    for (const Placed& p : placed_) {
        const Entry& e = models_[p.model];
        if (!e.ready) {
            continue;
        }
        const DVec3 offset = p.origin_ecef - view.eye_ecef;
        const glm::dvec4 c = m * glm::dvec4(offset + p.rotation * e.centre, 1.0);
        const double r = e.radius_m;
        // Toward the light (z under 0) casters still count: clamped onto
        // the box.
        if (std::abs(c.x) > 1.0 + r * sx || std::abs(c.y) > 1.0 + r * sy || c.z > 1.0 + r * sz) {
            continue;
        }
        if (n + e.primitives.size() > kMaxModelDraws) {
            ++dropped;
            continue;
        }
        ModelDraw d;
        for (int row = 0; row < 3; ++row) {
            d.rows[static_cast<std::size_t>(row)] = {
                static_cast<float>(p.rotation[0][row]), static_cast<float>(p.rotation[1][row]),
                static_cast<float>(p.rotation[2][row]), static_cast<float>(offset[row])};
        }
        for (const Primitive& prim : e.primitives) {
            d.material = prim.material;
            records[n] = d;
            commands[n] = {prim.index_count, 1, prim.first_index, e.vertex_offset, base + n};
            ++n;
        }
    }
    if (n == 0) {
        return;
    }
    (void)vmaFlushAllocation(gpu_.allocator, draws_.memory, base * sizeof(ModelDraw),
                             n * sizeof(ModelDraw));
    (void)vmaFlushAllocation(gpu_.allocator, commands_.memory,
                             base * sizeof(VkDrawIndexedIndirectCommand),
                             n * sizeof(VkDrawIndexedIndirectCommand));
    ModelPush push;
    push.ecef_to_clip = to_map;
    push.samples = 1;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    const VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertices_.buffer, &zero);
    vkCmdBindIndexBuffer(cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    vkCmdDrawIndexedIndirect(cmd, commands_.buffer, base * sizeof(VkDrawIndexedIndirectCommand), n,
                             sizeof(VkDrawIndexedIndirectCommand));
}

}  // namespace fovea
