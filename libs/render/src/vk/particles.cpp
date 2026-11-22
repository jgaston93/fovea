#include "particles.hpp"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

#include "fovea/render/particle_noise.hpp"

namespace fovea {

namespace {

// Each slot's buffer: the origins, the light space, then the particles.
constexpr VkDeviceSize kOriginsBytes = sizeof(GpuOrigin) * kMaxParticleOrigins;
constexpr VkDeviceSize kLightSpaceAt = kOriginsBytes;
constexpr VkDeviceSize kParticlesAt = kOriginsBytes + 256;
constexpr VkDeviceSize kVolumeSpaceAt = kLightSpaceAt + 64;  // D-052
static_assert(kVolumeSpaceAt + sizeof(GpuVolumeSpace) <= kParticlesAt);
static_assert(kOriginsBytes % 256 == 0 && kParticlesAt % 256 == 0,
              "offsets must suit any storage alignment");
static_assert(sizeof(GpuLightSpace) <= 256);

constexpr VkFormat kParticleImageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// The light buffer: 32-bit floats where the GPU blends and filters them (a
// thick cloud of thin puffs sums small optical depths, which half floats'
// 11 bits stop adding near 4 to 8), else 16-bit, which every GPU does.
VkFormat light_format_for(VkPhysicalDevice physical) {
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_R32_SFLOAT, &p);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                                      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    return (p.optimalTilingFeatures & need) == need ? VK_FORMAT_R32_SFLOAT : VK_FORMAT_R16_SFLOAT;
}

// The volume's float images (D-052): written by compute, sampled filtered:
// 32-bit where the GPU does both, else 16-bit (most do both at 32).
// The volume's density and sun images: written and read as storage images
// the shaders declare without a format (Slang's RWTexture3D<float>), and
// filtered when sampled. R32F where it has all of that, else R16F; neither,
// and the volume can't be drawn here.
VkFormat volume_format_for(VkPhysicalDevice physical) {
    const VkFormatFeatureFlags2 need = VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
                                       VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                                       VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT |
                                       VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    for (const VkFormat f : {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16_SFLOAT}) {
        VkFormatProperties3 p3{};
        p3.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3;
        VkFormatProperties2 p{};
        p.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
        p.pNext = &p3;
        vkGetPhysicalDeviceFormatProperties2(physical, f, &p);
        if ((p3.optimalTilingFeatures & need) == need) {
            return f;
        }
    }
    throw std::runtime_error(
        "particles: no float format this GPU can store to without a format and filter, for the "
        "volume ([particles] renderer = \"volume\"); use \"puffs\"");
}

constexpr VkShaderStageFlags kAllStages =
    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

// Mirrors ParticlePush in shaders/particles.slang.
struct ParticlePush {
    Mat4 ecef_to_clip{1.0F};
    std::array<float, 3> anchor{};  // the origins' anchor relative to the eye
    float forward_scattering = 0.0F;
    std::array<float, 3> albedo{};
    std::uint32_t order_base = 0;   // this view's first entry in the order buffer
    std::uint32_t depth_scale = 1;  // scene depth pixels per particle image pixel: 1 or 2
    float unused = 0.0F;
    std::array<float, 2> inv_target{};  // 1 / the view's render size, pixels (the composite)
    float time = 0.0F;                  // seconds the detail has drifted (D-043)
    float detail = 0.0F;
    float detail_size = 1.0F;
    float swirl = 0.0F;
};
static_assert(sizeof(ParticlePush) == 128, "must match ParticlePush in particles.slang");

// The detail volumes (D-043): 32^3 each, the size Schneider and Vos give
// their high-frequency detail noise; 4 cells a side at the first of 3
// octaves.
constexpr int kVolumeSize = 32;
constexpr int kVolumeCells = 4;
constexpr int kVolumeOctaves = 3;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("particles: " + what);
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fail(std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")");
    }
}

enum class Blend : std::uint8_t {
    over,       // premultiplied, back to front: the eye's slices, reversed view
    under,      // premultiplied, front to back (Green 2013: one_minus_dest_alpha, one)
    add,        // optical depths summed: the light buffer
    composite,  // the particle image over the scene, the scene's alpha kept
};

VkPipeline make_compute(VkDevice device, VkPipelineLayout layout, VkShaderModule module) {
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &p),
          "vkCreateComputePipelines (particles' volume)");
    return p;
}

VkPipeline make_pipeline(VkDevice device, VkPipelineLayout layout, VkShaderModule vertex,
                         VkShaderModule fragment, VkFormat format, Blend mode) {
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
    VkPipelineColorBlendAttachmentState b{};
    b.blendEnable = VK_TRUE;
    b.colorBlendOp = VK_BLEND_OP_ADD;
    b.alphaBlendOp = VK_BLEND_OP_ADD;
    b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    switch (mode) {
        case Blend::over:
            b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case Blend::under:
            b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case Blend::add:
            b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
            break;
        case Blend::composite:
            b.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            b.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
            b.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
    }
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &b;
    const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                       VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
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

// Begins rendering into one colour image, the whole of it, and sets the
// viewport and scissor to it.
void begin(VkCommandBuffer cmd, VkImageView view, VkImageLayout layout, VkExtent2D extent,
           VkAttachmentLoadOp load) {
    VkRenderingAttachmentInfo colour{};
    colour.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colour.imageView = view;
    colour.imageLayout = layout;
    colour.loadOp = load;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, extent};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &colour;
    vkCmdBeginRendering(cmd, &info);
    const VkViewport viewport{
        0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height),
        0.0F, 1.0F};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

VkImageMemoryBarrier2 image_barrier(VkImage image, VkPipelineStageFlags2 src_stage,
                                    VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                                    VkAccessFlags2 dst_access, VkImageLayout from,
                                    VkImageLayout to) {
    VkImageMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b.srcStageMask = src_stage;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst_stage;
    b.dstAccessMask = dst_access;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

void barriers(VkCommandBuffer cmd, std::span<const VkImageMemoryBarrier2> bs) {
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = static_cast<std::uint32_t>(bs.size());
    dep.pImageMemoryBarriers = bs.data();
    vkCmdPipelineBarrier2(cmd, &dep);
}

constexpr VkPipelineStageFlags2 kColourStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
constexpr VkPipelineStageFlags2 kFragmentStage = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
constexpr VkAccessFlags2 kColourWrite = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
constexpr VkAccessFlags2 kColourReadWrite =
    VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
constexpr VkAccessFlags2 kSampled = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

}  // namespace

ParticlesGpu::Image ParticlesGpu::make_image(VkExtent2D extent, VkFormat format) const {
    Image i;
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {extent.width, extent.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    check(vmaCreateImage(gpu_.allocator, &ici, &aci, &i.image, &i.memory, nullptr),
          "vmaCreateImage (particles)");
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = i.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(gpu_.device, &vci, nullptr, &i.view), "vkCreateImageView (particles)");
    return i;
}

void ParticlesGpu::destroy_image(Image& i) const {
    vkDestroyImageView(gpu_.device, i.view, nullptr);
    if (i.image != VK_NULL_HANDLE) {
        vmaDestroyImage(gpu_.allocator, i.image, i.memory);
    }
    i = Image{};
}

ParticlesGpu::Image ParticlesGpu::make_volume_target(std::uint32_t size, VkFormat format) const {
    Image i;
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_3D;
    ici.format = format;
    ici.extent = {size, size, size};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    check(vmaCreateImage(gpu_.allocator, &ici, &aci, &i.image, &i.memory, nullptr),
          "vmaCreateImage (particles' volume target)");
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = i.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    check(vkCreateImageView(gpu_.device, &vci, nullptr, &i.view),
          "vkCreateImageView (particles' volume target)");
    return i;
}

ParticlesGpu::Image ParticlesGpu::make_volume(std::uint32_t size, VkFormat format,
                                              std::span<const std::byte> bytes) const {
    Image i;
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_3D;
    ici.format = format;
    ici.extent = {size, size, size};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    check(vmaCreateImage(gpu_.allocator, &ici, &aci, &i.image, &i.memory, nullptr),
          "vmaCreateImage (particles' volume)");

    // Once, at start: a staging buffer, a command buffer of its own, waited
    // for (the renderer is being made; nothing else is on the queue).
    VkBuffer staging = VK_NULL_HANDLE;
    VmaAllocation staging_memory = nullptr;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    const auto cleanup = [&] {
        vkDestroyFence(gpu_.device, fence, nullptr);
        vkDestroyCommandPool(gpu_.device, pool, nullptr);
        if (staging != VK_NULL_HANDLE) {
            vmaDestroyBuffer(gpu_.allocator, staging, staging_memory);
        }
    };
    try {
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = i.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
        vci.format = format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vkCreateImageView(gpu_.device, &vci, nullptr, &i.view),
              "vkCreateImageView (particles' volume)");
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = bytes.size();
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo sci{};
        sci.usage = VMA_MEMORY_USAGE_AUTO;
        sci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        check(vmaCreateBuffer(gpu_.allocator, &bci, &sci, &staging, &staging_memory, &info),
              "vmaCreateBuffer (particles' volume staging)");
        std::memcpy(info.pMappedData, bytes.data(), bytes.size());
        (void)vmaFlushAllocation(gpu_.allocator, staging_memory, 0, VK_WHOLE_SIZE);
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = gpu_.queue_family;
        check(vkCreateCommandPool(gpu_.device, &pci, nullptr, &pool),
              "vkCreateCommandPool (particles)");
        VkCommandBufferAllocateInfo cai{};
        cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cai.commandPool = pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        check(vkAllocateCommandBuffers(gpu_.device, &cai, &cmd),
              "vkAllocateCommandBuffers (particles)");
        VkCommandBufferBeginInfo begin_info{};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cmd, &begin_info), "vkBeginCommandBuffer (particles)");
        const std::array<VkImageMemoryBarrier2, 1> to_copy{
            image_barrier(i.image, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                          VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)};
        barriers(cmd, to_copy);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {size, size, size};
        vkCmdCopyBufferToImage(cmd, staging, i.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        const std::array<VkImageMemoryBarrier2, 1> to_read{
            image_barrier(i.image, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          kFragmentStage, kSampled, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
        barriers(cmd, to_read);
        check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer (particles)");
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(gpu_.device, &fci, nullptr, &fence), "vkCreateFence (particles)");
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        check(vkQueueSubmit(gpu_.queue, 1, &si, fence), "vkQueueSubmit (particles' volume)");
        check(vkWaitForFences(gpu_.device, 1, &fence, VK_TRUE, UINT64_MAX),
              "vkWaitForFences (particles' volume)");
    } catch (...) {
        cleanup();
        destroy_image(i);
        throw;
    }
    cleanup();
    return i;
}

ParticlesGpu::ParticlesGpu(const GpuContext& gpu, std::size_t capacity, std::uint32_t slots,
                           std::span<const VkExtent2D> extents, VkFormat colour,
                           const Modules& modules, const ParticleQuality& quality)
    : gpu_(gpu),
      capacity_(capacity),
      quality_(quality),
      slots_(slots),
      counts_(slots, 0),
      sorted_views_(slots, 0),
      anchors_(slots, DVec3(0.0)),
      times_(slots, 0.0F),
      front_to_back_(std::size_t{slots} * extents.size(), 1),
      volume_pending_(slots, 0),
      view_sets_(extents.size(), VK_NULL_HANDLE),
      packed_(capacity),
      packed_origins_(kMaxParticleOrigins),
      centres_(capacity),
      keys_(capacity * 2),
      scratch_(capacity * 2) {
    if (capacity == 0 || slots == 0 || extents.empty() || quality.slices == 0 ||
        quality.light_buffer == 0) {
        fail("no capacity, slots, outputs, slices or light buffer");
    }
    const auto outputs = static_cast<std::uint32_t>(extents.size());
    try {
        light_format_ = light_format_for(gpu_.physical);
        for (const VkExtent2D e : extents) {
            const VkExtent2D size =
                quality_.half_resolution ? VkExtent2D{(e.width + 1) / 2, (e.height + 1) / 2} : e;
            image_extents_.push_back(size);
            particle_images_.push_back(make_image(size, kParticleImageFormat));
        }
        light_ = make_image({quality_.light_buffer, quality_.light_buffer}, light_format_);
        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(gpu_.device, &sci, nullptr, &light_sampler_),
              "vkCreateSampler (particles)");
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        check(vkCreateSampler(gpu_.device, &sci, nullptr, &noise_sampler_),
              "vkCreateSampler (particles' noise)");
        {
            // R8 unorm and RGBA16F: formats every Vulkan GPU samples with
            // linear filtering.
            const std::vector<float> detail =
                detail_volume(kVolumeSize, kVolumeCells, kVolumeOctaves);
            std::vector<std::uint8_t> d8(detail.size());
            for (std::size_t k = 0; k < detail.size(); ++k) {
                d8[k] = static_cast<std::uint8_t>(
                    std::lround(std::clamp(detail[k], 0.0F, 1.0F) * 255.0F));
            }
            detail_ = make_volume(kVolumeSize, VK_FORMAT_R8_UNORM, std::as_bytes(std::span(d8)));
            for (const std::uint8_t v : d8) {
                ++detail_histogram_.at(v);
            }
            const std::vector<Vec3> curl = curl_volume(kVolumeSize, kVolumeCells, kVolumeOctaves);
            std::vector<std::uint16_t> c16(curl.size() * 4);
            for (std::size_t k = 0; k < curl.size(); ++k) {
                for (int c = 0; c < 3; ++c) {
                    c16[k * 4 + static_cast<std::size_t>(c)] = glm::packHalf1x16(curl[k][c]);
                }
                c16[k * 4 + 3] = glm::packHalf1x16(0.0F);
            }
            curl_ = make_volume(kVolumeSize, VK_FORMAT_R16G16B16A16_SFLOAT,
                                std::as_bytes(std::span(c16)));
        }

        const VkDeviceSize particles_bytes = sizeof(GpuParticle) * capacity;
        const VkDeviceSize order_bytes = sizeof(std::uint32_t) * capacity * outputs;
        const auto host_buffer = [this](VkDeviceSize size, VkBuffer& buffer, VmaAllocation& memory,
                                        void*& mapped) {
            VkBufferCreateInfo bci{};
            bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bci.size = size;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_AUTO;
            aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                        VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info{};
            check(vmaCreateBuffer(gpu_.allocator, &bci, &aci, &buffer, &memory, &info),
                  "vmaCreateBuffer (particles)");
            if (info.pMappedData == nullptr) {
                fail("vmaCreateBuffer (particles): not mapped");
            }
            mapped = info.pMappedData;
        };
        for (Slot& s : slots_) {
            host_buffer(kParticlesAt + particles_bytes, s.buffer, s.memory, s.mapped);
            host_buffer(order_bytes, s.order, s.order_memory, s.order_mapped);
        }

        std::array<VkDescriptorSetLayoutBinding, 5> buffer_bindings{};
        for (std::uint32_t b = 0; b < buffer_bindings.size(); ++b) {
            buffer_bindings.at(b) = {b, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, kAllStages, nullptr};
        }
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = static_cast<std::uint32_t>(buffer_bindings.size());
        lci.pBindings = buffer_bindings.data();
        check(vkCreateDescriptorSetLayout(gpu_.device, &lci, nullptr, &buffers_layout_),
              "vkCreateDescriptorSetLayout (particles)");
        const std::array<VkDescriptorSetLayoutBinding, 9> view_bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{6, VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{8, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            VkDescriptorSetLayoutBinding{9, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        lci.bindingCount = static_cast<std::uint32_t>(view_bindings.size());
        lci.pBindings = view_bindings.data();
        check(vkCreateDescriptorSetLayout(gpu_.device, &lci, nullptr, &view_layout_),
              "vkCreateDescriptorSetLayout (particles' view)");

        // The volume's three images (D-052), 1 voxel when it isn't drawn.
        // Off, R32F (storage is mandatory for it): never fails for the puffs.
        const VkFormat volume_format =
            quality_.volume ? volume_format_for(gpu_.physical) : VK_FORMAT_R32_SFLOAT;
        const std::uint32_t vsize = quality_.volume ? quality_.volume_size : 1;
        const std::uint32_t lsize = quality_.volume ? quality_.volume_light_size : 1;
        volume_accumulate_ = make_volume_target(vsize, VK_FORMAT_R32_UINT);
        volume_density_ = make_volume_target(vsize, volume_format);
        volume_sun_ = make_volume_target(lsize, volume_format);
        const std::array<VkDescriptorSetLayoutBinding, 3> volume_bindings{
            VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                         VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        lci.bindingCount = static_cast<std::uint32_t>(volume_bindings.size());
        lci.pBindings = volume_bindings.data();
        check(vkCreateDescriptorSetLayout(gpu_.device, &lci, nullptr, &volume_layout_),
              "vkCreateDescriptorSetLayout (particles' volume)");

        const std::array<VkDescriptorPoolSize, 4> sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 * slots},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 7 * outputs},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 2 * outputs},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3}};
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = slots + outputs + 1;
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
            const std::array<VkDescriptorBufferInfo, 5> infos{
                VkDescriptorBufferInfo{s.buffer, kParticlesAt, particles_bytes},
                VkDescriptorBufferInfo{s.buffer, 0, kOriginsBytes},
                VkDescriptorBufferInfo{s.buffer, kLightSpaceAt, sizeof(GpuLightSpace)},
                VkDescriptorBufferInfo{s.order, 0, order_bytes},
                VkDescriptorBufferInfo{s.buffer, kVolumeSpaceAt, sizeof(GpuVolumeSpace)}};
            std::array<VkWriteDescriptorSet, 5> writes{};
            for (std::uint32_t b = 0; b < writes.size(); ++b) {
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
        for (std::uint32_t o = 0; o < outputs; ++o) {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = pool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &view_layout_;
            check(vkAllocateDescriptorSets(gpu_.device, &ai, &view_sets_.at(o)),
                  "vkAllocateDescriptorSets (particles' view)");
            // The light buffer is read and written in GENERAL (D-041: no
            // transitions between slices); the particle image is read once
            // it's done.
            const VkDescriptorImageInfo light_info{VK_NULL_HANDLE, light_.view,
                                                   VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo sampler_info{light_sampler_, VK_NULL_HANDLE,
                                                     VK_IMAGE_LAYOUT_UNDEFINED};
            const VkDescriptorImageInfo image_info{VK_NULL_HANDLE, particle_images_.at(o).view,
                                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorImageInfo detail_info{VK_NULL_HANDLE, detail_.view,
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorImageInfo curl_info{VK_NULL_HANDLE, curl_.view,
                                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            const VkDescriptorImageInfo noise_sampler_info{noise_sampler_, VK_NULL_HANDLE,
                                                           VK_IMAGE_LAYOUT_UNDEFINED};
            const VkDescriptorImageInfo volume_info{VK_NULL_HANDLE, volume_density_.view,
                                                    VK_IMAGE_LAYOUT_GENERAL};
            const VkDescriptorImageInfo volume_sun_info{VK_NULL_HANDLE, volume_sun_.view,
                                                        VK_IMAGE_LAYOUT_GENERAL};
            std::array<VkWriteDescriptorSet, 8> writes{};
            for (VkWriteDescriptorSet& w : writes) {
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = view_sets_.at(o);
                w.descriptorCount = 1;
            }
            writes[3].dstBinding = 4;
            writes[3].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[3].pImageInfo = &detail_info;
            writes[4].dstBinding = 5;
            writes[4].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[4].pImageInfo = &curl_info;
            writes[5].dstBinding = 6;
            writes[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[5].pImageInfo = &noise_sampler_info;
            writes[0].dstBinding = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[0].pImageInfo = &light_info;
            writes[1].dstBinding = 2;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            writes[1].pImageInfo = &sampler_info;
            writes[2].dstBinding = 3;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[2].pImageInfo = &image_info;
            writes[6].dstBinding = 8;
            writes[6].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[6].pImageInfo = &volume_info;
            writes[7].dstBinding = 9;
            writes[7].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            writes[7].pImageInfo = &volume_sun_info;
            vkUpdateDescriptorSets(gpu_.device, static_cast<std::uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }

        {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = pool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &volume_layout_;
            check(vkAllocateDescriptorSets(gpu_.device, &ai, &volume_set_),
                  "vkAllocateDescriptorSets (particles' volume)");
            const std::array<VkDescriptorImageInfo, 3> infos{
                VkDescriptorImageInfo{VK_NULL_HANDLE, volume_accumulate_.view,
                                      VK_IMAGE_LAYOUT_GENERAL},
                VkDescriptorImageInfo{VK_NULL_HANDLE, volume_density_.view,
                                      VK_IMAGE_LAYOUT_GENERAL},
                VkDescriptorImageInfo{VK_NULL_HANDLE, volume_sun_.view, VK_IMAGE_LAYOUT_GENERAL}};
            std::array<VkWriteDescriptorSet, 3> writes{};
            for (std::uint32_t b = 0; b < writes.size(); ++b) {
                writes.at(b).sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes.at(b).dstSet = volume_set_;
                writes.at(b).dstBinding = b;
                writes.at(b).descriptorCount = 1;
                writes.at(b).descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes.at(b).pImageInfo = &infos.at(b);
            }
            vkUpdateDescriptorSets(gpu_.device, static_cast<std::uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }

        // Set 0 the buffers, set 1 the frame set (the view's lighting), set 2
        // the view's images, set 3 the volume's (D-052).
        const std::array<VkDescriptorSetLayout, 4> layouts{buffers_layout_, gpu_.frame_layout,
                                                           view_layout_, volume_layout_};
        const VkPushConstantRange push{kAllStages, 0, sizeof(ParticlePush)};
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = static_cast<std::uint32_t>(layouts.size());
        plci.pSetLayouts = layouts.data();
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(gpu_.device, &plci, nullptr, &layout_),
              "vkCreatePipelineLayout (particles)");
        over_ = make_pipeline(gpu_.device, layout_, modules.vs_particles, modules.fs_particles,
                              kParticleImageFormat, Blend::over);
        under_ = make_pipeline(gpu_.device, layout_, modules.vs_particles, modules.fs_particles,
                               kParticleImageFormat, Blend::under);
        light_pipeline_ = make_pipeline(gpu_.device, layout_, modules.vs_light, modules.fs_light,
                                        light_format_, Blend::add);
        composite_ = make_pipeline(gpu_.device, layout_, modules.vs_composite, modules.fs_composite,
                                   colour, Blend::composite);
        if (quality_.volume) {
            volume_inject_ = make_compute(gpu_.device, layout_, modules.cs_volume_inject);
            volume_convert_ = make_compute(gpu_.device, layout_, modules.cs_volume_convert);
            volume_sun_pass_ = make_compute(gpu_.device, layout_, modules.cs_volume_sun);
            // Onto a cleared particle image: over is a copy.
            volume_march_ = make_pipeline(gpu_.device, layout_, modules.vs_volume,
                                          modules.fs_volume, kParticleImageFormat, Blend::over);
        }
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
    for (VkPipeline p : {over_, under_, light_pipeline_, composite_, volume_inject_,
                         volume_convert_, volume_sun_pass_, volume_march_}) {
        vkDestroyPipeline(gpu_.device, p, nullptr);
    }
    vkDestroyPipelineLayout(gpu_.device, layout_, nullptr);
    vkDestroyDescriptorPool(gpu_.device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(gpu_.device, buffers_layout_, nullptr);
    vkDestroyDescriptorSetLayout(gpu_.device, view_layout_, nullptr);
    vkDestroySampler(gpu_.device, light_sampler_, nullptr);
    vkDestroySampler(gpu_.device, noise_sampler_, nullptr);
    destroy_image(detail_);
    destroy_image(curl_);
    destroy_image(volume_accumulate_);
    destroy_image(volume_density_);
    destroy_image(volume_sun_);
    vkDestroyDescriptorSetLayout(gpu_.device, volume_layout_, nullptr);
    for (Image& i : particle_images_) {
        destroy_image(i);
    }
    destroy_image(light_);
    for (Slot& s : slots_) {
        if (s.buffer != VK_NULL_HANDLE) {
            vmaDestroyBuffer(gpu_.allocator, s.buffer, s.memory);
        }
        if (s.order != VK_NULL_HANDLE) {
            vmaDestroyBuffer(gpu_.allocator, s.order, s.order_memory);
        }
    }
    gpu_.device = VK_NULL_HANDLE;
}

void ParticlesGpu::set_depth(std::uint32_t output, VkImageView depth) {
    const VkDescriptorImageInfo info{VK_NULL_HANDLE, depth,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = view_sets_.at(output);
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &info;
    vkUpdateDescriptorSets(gpu_.device, 1, &w, 0, nullptr);
}

std::size_t ParticlesGpu::pack(std::uint32_t slot, const ParticleFrame& frame, LocalTime at,
                               const ParticleLook& look, const DVec3& anchor,
                               std::span<const SceneView> views, const DVec3& sun) noexcept {
    Slot& s = slots_[slot];
    auto* base = static_cast<std::uint8_t*>(s.mapped);
    // Packed in the CPU's memory, read there, then copied (D-049).
    const std::span<GpuOrigin> origins(packed_origins_);
    const std::span<GpuParticle> particles(packed_);
    const std::size_t n = pack_particles(frame, at, look, anchor, particles, origins);
    const std::span<const GpuParticle> packed = particles.first(n);
    std::memcpy(base, origins.data(), origins.size_bytes());
    std::memcpy(base + kParticlesAt, packed.data(), packed.size_bytes());
    const std::span<Vec3> centres = std::span(centres_).first(n);
    particle_centres(packed, origins, centres);
    const Vec3 travel =
        -Vec3(static_cast<float>(sun.x), static_cast<float>(sun.y), static_cast<float>(sun.z));
    GpuLightSpace ls = light_space(centres, packed, travel);
    ls.erosion = look.erosion;  // D-051
    if (look.detail != gain_detail_ || look.erosion != gain_erosion_) {
        erosion_gain_ = quality_.volume
                            ? volume_erosion_gain(detail_histogram_, look.detail, look.erosion)
                            : erosion_gain(detail_histogram_, look.detail, look.erosion);
        gain_detail_ = look.detail;
        gain_erosion_ = look.erosion;
    }
    ls.erosion_gain = erosion_gain_;
    std::memcpy(base + kLightSpaceAt, &ls, sizeof(ls));
    // The volume's box (D-052), when it's drawn.
    GpuVolumeSpace vs;
    if (quality_.volume) {
        vs = volume_space(centres, packed, quality_.volume_size, quality_.volume_light_size,
                          quality_.volume_max_m, anchor);
        vs.particles = static_cast<std::uint32_t>(n);
    }
    std::memcpy(base + kVolumeSpaceAt, &vs, sizeof(vs));
    volume_pending_[slot] = quality_.volume && n > 0 ? 1 : 0;
    const std::size_t outputs = view_sets_.size();
    auto* order = static_cast<std::uint32_t*>(s.order_mapped);
    for (std::size_t o = 0; o < outputs && o < views.size(); ++o) {
        const HalfAngle h = half_angle(view_centre_ray(views[o].ecef_to_clip), travel);
        sort_along(centres, h.axis, keys_, scratch_, std::span(order + o * capacity_, n));
        front_to_back_[slot * outputs + o] = h.front_to_back ? 1 : 0;
    }
    (void)vmaFlushAllocation(gpu_.allocator, s.memory, 0, kParticlesAt + sizeof(GpuParticle) * n);
    (void)vmaFlushAllocation(gpu_.allocator, s.order_memory, 0, VK_WHOLE_SIZE);
    counts_[slot] = n;
    sorted_views_[slot] = std::min(outputs, views.size());
    anchors_[slot] = anchor;
    // The detail's clock: seconds, wrapped every 1000 so a float keeps
    // their fractions. The wrap is seamless: the drift is 0.03 tiles a
    // second, 30 whole tiles of the wrapping volume in 1000 s.
    const double seconds = std::chrono::duration<double>(at.time_since_epoch()).count();
    times_[slot] = static_cast<float>(std::fmod(seconds, 1000.0));
    return n;
}

void ParticlesGpu::record(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t output,
                          const SceneView& view, std::uint32_t lighting_offset,
                          const ParticleLook& look, VkImageView target, VkExtent2D extent) const {
    const std::size_t n = counts_[slot];
    if (n == 0 || output >= sorted_views_[slot]) {
        return;  // nothing, or a view pack wasn't given (its order unsorted)
    }
    const Image& image = particle_images_.at(output);
    ParticlePush push;
    push.ecef_to_clip = view.ecef_to_clip;
    push.anchor = floats3(anchors_[slot] - view.eye_ecef);
    push.forward_scattering = look.forward_scattering;
    push.albedo = look.albedo;
    push.order_base = static_cast<std::uint32_t>(output * capacity_);
    push.depth_scale = quality_.half_resolution ? 2U : 1U;
    push.time = times_[slot];
    push.detail = look.detail;
    push.detail_size = look.detail_size;
    push.swirl = look.swirl;
    push.inv_target = {1.0F / static_cast<float>(extent.width),
                       1.0F / static_cast<float>(extent.height)};
    const VkExtent2D image_extent = image_extents_.at(output);
    const VkExtent2D light_extent{quality_.light_buffer, quality_.light_buffer};
    const auto bind = [&](VkPipeline p) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        const std::array<VkDescriptorSet, 4> sets{slots_[slot].set, gpu_.frame_set,
                                                  view_sets_.at(output), volume_set_};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0,
                                static_cast<std::uint32_t>(sets.size()), sets.data(), 1,
                                &lighting_offset);
        vkCmdPushConstants(cmd, layout_, kAllStages, 0, sizeof(push), &push);
    };

    // Drawn as a volume (D-052): the volume made once a frame (at the first
    // view), then each view marched through it into its particle image,
    // composited as the puffs are.
    if (quality_.volume) {
        if (volume_pending_[slot] != 0) {
            record_volume(cmd, slot, lighting_offset);
            volume_pending_[slot] = 0;
        }
        const std::array<VkImageMemoryBarrier2, 1> to_image{
            image_barrier(image.image, kFragmentStage, 0, kColourStage, kColourReadWrite,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)};
        barriers(cmd, to_image);
        begin(cmd, image.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, image_extent,
              VK_ATTACHMENT_LOAD_OP_CLEAR);
        bind(volume_march_);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        const std::array<VkImageMemoryBarrier2, 1> done{image_barrier(
            image.image, kColourStage, kColourWrite, kFragmentStage, kSampled,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
        barriers(cmd, done);
        begin(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, extent,
              VK_ATTACHMENT_LOAD_OP_LOAD);
        bind(composite_);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRendering(cmd);
        return;
    }

    // Both images start over: the particle image after the previous frame's
    // composite read it, the light buffer after the previous view used it.
    const std::array<VkImageMemoryBarrier2, 2> start{
        image_barrier(image.image, kFragmentStage, 0, kColourStage, kColourReadWrite,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
        image_barrier(light_.image, kFragmentStage | kColourStage, kColourWrite, kColourStage,
                      kColourReadWrite, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)};
    barriers(cmd, start);
    begin(cmd, light_.view, VK_IMAGE_LAYOUT_GENERAL, light_extent,
          VK_ATTACHMENT_LOAD_OP_CLEAR);  // no light path yet: optical depth 0
    vkCmdEndRendering(cmd);

    const bool front_to_back = front_to_back_[std::size_t{slot} * view_sets_.size() + output] != 0;
    // Without self-shadowing: one slice, no light pass (the buffer stays 0).
    const std::size_t slices = quality_.self_shadowing ? quality_.slices : 1;
    const std::size_t per_slice = (n + slices - 1) / slices;
    for (std::size_t first = 0; first < n; first += per_slice) {
        const auto count = static_cast<std::uint32_t>(std::min(per_slice, n - first));
        // The eye's slice reads the light buffer the slices nearer the
        // light left, and adds to the particle image.
        const std::array<VkImageMemoryBarrier2, 2> to_eye{
            image_barrier(light_.image, kColourStage, kColourWrite, kFragmentStage, kSampled,
                          VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
            image_barrier(image.image, kColourStage, kColourWrite, kColourStage, kColourReadWrite,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                          VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)};
        barriers(cmd, to_eye);
        begin(cmd, image.view, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, image_extent,
              first == 0 ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD);
        bind(front_to_back ? under_ : over_);
        vkCmdDraw(cmd, count * 6, 1, static_cast<std::uint32_t>(first * 6), 0);
        vkCmdEndRendering(cmd);
        if (first + count >= n) {
            break;  // nothing after the last slice to shadow
        }
        // Then into the light buffer, once the eye's slice has read it.
        const std::array<VkImageMemoryBarrier2, 1> to_light{
            image_barrier(light_.image, kFragmentStage | kColourStage, kColourWrite, kColourStage,
                          kColourReadWrite, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL)};
        barriers(cmd, to_light);
        begin(cmd, light_.view, VK_IMAGE_LAYOUT_GENERAL, light_extent, VK_ATTACHMENT_LOAD_OP_LOAD);
        bind(light_pipeline_);
        vkCmdDraw(cmd, count * 6, 1, static_cast<std::uint32_t>(first * 6), 0);
        vkCmdEndRendering(cmd);
    }

    // The particle image over the scene.
    const std::array<VkImageMemoryBarrier2, 1> done{image_barrier(
        image.image, kColourStage, kColourWrite, kFragmentStage, kSampled,
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)};
    barriers(cmd, done);
    begin(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, extent,
          VK_ATTACHMENT_LOAD_OP_LOAD);
    bind(composite_);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

// The volume, once a frame (D-052): cleared, the particles added in fixed
// point, converted to extinction, the optical depth toward the sun marched;
// then sampled by every view's march. Every image in GENERAL.
void ParticlesGpu::record_volume(VkCommandBuffer cmd, std::uint32_t slot,
                                 std::uint32_t lighting_offset) const {
    constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr VkPipelineStageFlags2 kTransfer = VK_PIPELINE_STAGE_2_CLEAR_BIT;
    constexpr VkAccessFlags2 kStorageRw =
        VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    const std::size_t n = counts_[slot];
    const std::uint32_t size = quality_.volume_size;
    const std::uint32_t light = quality_.volume_light_size;
    // Last frame's views read them (fragment): this one rewrites them.
    const std::array<VkImageMemoryBarrier2, 3> start{
        image_barrier(volume_accumulate_.image, kCompute, kStorageRw, kTransfer,
                      VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_GENERAL),
        image_barrier(volume_density_.image, kFragmentStage | kCompute, kSampled | kStorageRw,
                      kCompute, kStorageRw, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL),
        image_barrier(volume_sun_.image, kFragmentStage | kCompute, kSampled | kStorageRw, kCompute,
                      kStorageRw, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL)};
    barriers(cmd, start);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, volume_accumulate_.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
    const std::array<VkImageMemoryBarrier2, 1> cleared{
        image_barrier(volume_accumulate_.image, kTransfer, VK_ACCESS_2_TRANSFER_WRITE_BIT, kCompute,
                      kStorageRw, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL)};
    barriers(cmd, cleared);
    const std::array<VkDescriptorSet, 4> sets{slots_[slot].set, gpu_.frame_set, view_sets_.at(0),
                                              volume_set_};
    const auto bind = [&](VkPipeline p) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0,
                                static_cast<std::uint32_t>(sets.size()), sets.data(), 1,
                                &lighting_offset);
    };
    if (n > 0) {
        bind(volume_inject_);
        const auto x = static_cast<std::uint32_t>(std::min<std::size_t>(n, 65535));
        const auto y = static_cast<std::uint32_t>((n + 65534) / 65535);
        vkCmdDispatch(cmd, x, y, 1);
    }
    const std::array<VkImageMemoryBarrier2, 1> added{
        image_barrier(volume_accumulate_.image, kCompute, kStorageRw, kCompute, kStorageRw,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL)};
    barriers(cmd, added);
    bind(volume_convert_);
    vkCmdDispatch(cmd, (size + 3) / 4, (size + 3) / 4, (size + 3) / 4);
    const std::array<VkImageMemoryBarrier2, 1> converted{
        image_barrier(volume_density_.image, kCompute, kStorageRw, kCompute, kStorageRw,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL)};
    barriers(cmd, converted);
    bind(volume_sun_pass_);
    vkCmdDispatch(cmd, (light + 3) / 4, (light + 3) / 4, (light + 3) / 4);
    const std::array<VkImageMemoryBarrier2, 2> ready{
        image_barrier(volume_density_.image, kCompute, kStorageRw, kFragmentStage, kSampled,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL),
        image_barrier(volume_sun_.image, kCompute, kStorageRw, kFragmentStage, kSampled,
                      VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL)};
    barriers(cmd, ready);
}

}  // namespace fovea
