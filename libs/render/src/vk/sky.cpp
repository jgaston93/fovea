#include "sky.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace fovea {

namespace {

constexpr VkFormat kTableFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

// Mirrors Atmosphere in shaders/include/atmosphere.slang (std140).
struct AtmosphereGpu {
    std::array<float, 3> rayleigh_scattering{};
    float bottom_radius = 0.0F;
    std::array<float, 3> ozone_absorption{};
    float top_radius = 0.0F;
    std::array<float, 3> ground_albedo{};
    float rayleigh_scale_height = 0.0F;
    std::array<float, 3> sun_illuminance{};
    float mie_scale_height = 0.0F;
    float mie_scattering = 0.0F;
    float mie_extinction = 0.0F;
    float mie_g = 0.0F;
    float ozone_centre = 0.0F;
    float ozone_half_width = 0.0F;
    float sun_angular_radius = 0.0F;
    std::array<float, 2> unused{};
};
static_assert(sizeof(AtmosphereGpu) == 96, "must match Atmosphere in atmosphere.slang");

// Mirrors SkyPush in shaders/sky.slang.
struct SkyPush {
    float eye_radius = 0.0F;
    float mu_s = 0.0F;
    float sun_up = 0.0F;
    std::uint32_t view = 0;
    std::uint32_t views = 0;
    float adapt = 1.0F;
    float key = 0.0F;
    float floor_illuminance = 0.0F;
};
static_assert(sizeof(SkyPush) == 32, "must match SkyPush in sky.slang");

// The exposure takes a level surface's light to pi: a white one shows as
// 1 before tone mapping. Below a ten-thousandth of the sun's illuminance
// (deep twilight) it stops rising, so night stays dark (D-031).
constexpr double kExposureKey = std::numbers::pi;
constexpr double kExposureFloor = 1e-4;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what);
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fail(std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")");
    }
}

std::array<float, 3> to_float(const std::array<double, 3>& v) {
    return {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
}

AtmosphereGpu to_gpu(const Atmosphere& a) {
    AtmosphereGpu g;
    g.rayleigh_scattering = to_float(a.rayleigh_scattering);
    g.bottom_radius = static_cast<float>(a.bottom_radius_km);
    g.ozone_absorption = to_float(a.ozone_absorption);
    g.top_radius = static_cast<float>(a.top_radius_km);
    g.ground_albedo = to_float(a.ground_albedo);
    g.rayleigh_scale_height = static_cast<float>(a.rayleigh_scale_height_km);
    g.sun_illuminance = to_float(a.sun_illuminance);
    g.mie_scale_height = static_cast<float>(a.mie_scale_height_km);
    g.mie_scattering = static_cast<float>(a.mie_scattering);
    g.mie_extinction = static_cast<float>(a.mie_extinction);
    g.mie_g = static_cast<float>(a.mie_g);
    g.ozone_centre = static_cast<float>(a.ozone_centre_km);
    g.ozone_half_width = static_cast<float>(a.ozone_half_width_km);
    g.sun_angular_radius = static_cast<float>(a.sun_angular_radius_deg * std::numbers::pi / 180.0);
    return g;
}

float half_to_float(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15U) & 1U;
    const std::uint32_t exponent = (h >> 10U) & 0x1FU;
    const std::uint32_t mantissa = h & 0x3FFU;
    double v = 0.0;
    if (exponent == 0) {
        v = std::ldexp(static_cast<double>(mantissa), -24);
    } else if (exponent == 31) {
        v = mantissa == 0 ? std::numeric_limits<double>::infinity()
                          : std::numeric_limits<double>::quiet_NaN();
    } else {
        v = std::ldexp(static_cast<double>(mantissa | 0x400U), static_cast<int>(exponent) - 25);
    }
    return static_cast<float>(sign != 0 ? -v : v);
}

}  // namespace

VkDeviceSize SkyGpu::atmosphere_size() {
    return sizeof(AtmosphereGpu);
}

SkyGpu::Image SkyGpu::make_image(std::uint32_t width, std::uint32_t height, std::uint32_t layers,
                                 bool array) const {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kTableFormat;
    ici.extent = {width, height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = layers;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    Image img;
    check(vmaCreateImage(gpu_.allocator, &ici, &aci, &img.image, &img.memory, nullptr),
          "vmaCreateImage (sky)");
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img.image;
    vci.viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vci.format = kTableFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    check(vkCreateImageView(gpu_.device, &vci, nullptr, &img.view), "vkCreateImageView (sky)");
    return img;
}

SkyGpu::Buffer SkyGpu::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) const {
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (host) {
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    Buffer b;
    VmaAllocationInfo info{};
    check(vmaCreateBuffer(gpu_.allocator, &bci, &aci, &b.buffer, &b.memory, &info),
          "vmaCreateBuffer (sky)");
    b.mapped = info.pMappedData;
    if (host && b.mapped == nullptr) {
        vmaDestroyBuffer(gpu_.allocator, b.buffer, b.memory);
        fail("vmaCreateBuffer (sky): a host buffer wasn't mapped");
    }
    return b;
}

void SkyGpu::destroy(Image& i) const {
    if (i.view != VK_NULL_HANDLE) {
        vkDestroyImageView(gpu_.device, i.view, nullptr);
    }
    if (i.image != VK_NULL_HANDLE) {
        vmaDestroyImage(gpu_.allocator, i.image, i.memory);
    }
    i = {};
}

void SkyGpu::destroy(Buffer& b) const {
    if (b.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(gpu_.allocator, b.buffer, b.memory);
    }
    b = {};
}

SkyGpu::SkyGpu(const GpuContext& gpu, std::uint32_t views, const Atmosphere& atmosphere,
               std::span<const VkShaderModule, 5> modules)
    : gpu_(gpu), views_(std::max(views, 1U)), atmosphere_params_(atmosphere) {
    try {
        transmittance_ = make_image(kTransmittanceWidth, kTransmittanceHeight, 1, false);
        multi_scattering_ = make_image(kMultiScatteringSize, kMultiScatteringSize, 1, false);
        // A layer a view, viewed as an array even for one.
        sky_view_ = make_image(kSkyViewWidth, kSkyViewHeight, views_, true);
        atmosphere_ = make_buffer(sizeof(AtmosphereGpu), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true);
        const AtmosphereGpu a = to_gpu(atmosphere_params_);
        std::memcpy(atmosphere_.mapped, &a, sizeof(a));
        (void)vmaFlushAllocation(gpu_.allocator, atmosphere_.memory, 0, VK_WHOLE_SIZE);
        view_sky_size_ = sizeof(ViewSky) * views_;
        view_sky_ = make_buffer(
            view_sky_size_, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            false);
        exposure_state_ = make_buffer(
            16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);

        VkSamplerCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        check(vkCreateSampler(gpu_.device, &sci, nullptr, &sampler_), "vkCreateSampler (sky)");

        // The bindings of shaders/sky.slang's set 0.
        const std::array<VkDescriptorType, 8> types = {
            VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         VK_DESCRIPTOR_TYPE_STORAGE_BUFFER};
        std::array<VkDescriptorSetLayoutBinding, 8> bindings{};
        for (std::uint32_t b = 0; b < bindings.size(); ++b) {
            bindings[b].binding = b;
            bindings[b].descriptorType = types[b];
            bindings[b].descriptorCount = 1;
            bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = static_cast<std::uint32_t>(bindings.size());
        lci.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(gpu_.device, &lci, nullptr, &set_layout_),
              "vkCreateDescriptorSetLayout (sky)");
        const std::array<VkDescriptorPoolSize, 4> sizes = {
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 1;
        pci.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
        pci.pPoolSizes = sizes.data();
        check(vkCreateDescriptorPool(gpu_.device, &pci, nullptr, &pool_),
              "vkCreateDescriptorPool (sky)");
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        check(vkAllocateDescriptorSets(gpu_.device, &ai, &set_), "vkAllocateDescriptorSets (sky)");

        const VkDescriptorBufferInfo atmosphere_info{atmosphere_.buffer, 0, sizeof(AtmosphereGpu)};
        const VkDescriptorBufferInfo view_sky_info{view_sky_.buffer, 0, view_sky_size_};
        const VkDescriptorBufferInfo state_info{exposure_state_.buffer, 0, 16};
        const std::array<VkDescriptorImageInfo, 5> images = {
            VkDescriptorImageInfo{VK_NULL_HANDLE, transmittance_.view, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{sampler_, transmittance_.view, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{VK_NULL_HANDLE, multi_scattering_.view, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{sampler_, multi_scattering_.view, VK_IMAGE_LAYOUT_GENERAL},
            VkDescriptorImageInfo{VK_NULL_HANDLE, sky_view_.view, VK_IMAGE_LAYOUT_GENERAL}};
        std::array<VkWriteDescriptorSet, 8> writes{};
        for (std::uint32_t b = 0; b < writes.size(); ++b) {
            writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[b].dstSet = set_;
            writes[b].dstBinding = b;
            writes[b].descriptorCount = 1;
            writes[b].descriptorType = types[b];
        }
        writes[0].pBufferInfo = &atmosphere_info;
        for (std::uint32_t b = 1; b <= 5; ++b) {
            writes[b].pImageInfo = &images[b - 1];
        }
        writes[6].pBufferInfo = &view_sky_info;
        writes[7].pBufferInfo = &state_info;
        vkUpdateDescriptorSets(gpu_.device, static_cast<std::uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);

        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SkyPush)};
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &set_layout_;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(gpu_.device, &plci, nullptr, &layout_),
              "vkCreatePipelineLayout (sky)");
        for (std::size_t i = 0; i < pipelines_.size(); ++i) {
            VkComputePipelineCreateInfo cpci{};
            cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            cpci.stage.module = modules[i];
            cpci.stage.pName = "main";
            cpci.layout = layout_;
            check(vkCreateComputePipelines(gpu_.device, VK_NULL_HANDLE, 1, &cpci, nullptr,
                                           &pipelines_[i]),
                  "vkCreateComputePipelines (sky)");
        }
    } catch (...) {
        release();
        throw;
    }
}

SkyGpu::~SkyGpu() {
    release();
}

void SkyGpu::release() {
    if (gpu_.device == VK_NULL_HANDLE) {
        return;
    }
    for (VkPipeline& p : pipelines_) {
        vkDestroyPipeline(gpu_.device, p, nullptr);
        p = VK_NULL_HANDLE;
    }
    vkDestroyPipelineLayout(gpu_.device, layout_, nullptr);
    vkDestroyDescriptorPool(gpu_.device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(gpu_.device, set_layout_, nullptr);
    vkDestroySampler(gpu_.device, sampler_, nullptr);
    destroy(transmittance_);
    destroy(multi_scattering_);
    destroy(sky_view_);
    destroy(atmosphere_);
    destroy(view_sky_);
    destroy(exposure_state_);
    gpu_.device = VK_NULL_HANDLE;
}

namespace {

void compute_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src, VkAccessFlags2 src_access,
                     VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    b.srcStageMask = src;
    b.srcAccessMask = src_access;
    b.dstStageMask = dst;
    b.dstAccessMask = dst_access;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

constexpr VkAccessFlags2 kComputeRw = VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                      VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

}  // namespace

void SkyGpu::record(VkCommandBuffer cmd, std::span<const SkyViewInput> views, float adapt) {
    const VkPipelineStageFlags2 compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    if (!started_) {
        // Every table lives in GENERAL: written by compute, sampled after.
        std::array<VkImageMemoryBarrier2, 3> ibs{};
        const std::array<VkImage, 3> images = {transmittance_.image, multi_scattering_.image,
                                               sky_view_.image};
        for (std::size_t i = 0; i < ibs.size(); ++i) {
            ibs[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            ibs[i].srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            ibs[i].dstStageMask = compute;
            ibs[i].dstAccessMask = kComputeRw;
            ibs[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ibs[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ibs[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ibs[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ibs[i].image = images[i];
            ibs[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                       VK_REMAINING_ARRAY_LAYERS};
        }
        VkDependencyInfo dep{};
        dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dep.imageMemoryBarrierCount = static_cast<std::uint32_t>(ibs.size());
        dep.pImageMemoryBarriers = ibs.data();
        vkCmdPipelineBarrier2(cmd, &dep);
        vkCmdFillBuffer(cmd, exposure_state_.buffer, 0, VK_WHOLE_SIZE, 0);
        compute_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        compute, kComputeRw);
        started_ = true;
    }
    // After the previous frame's scene read the sky-view tables and light,
    // and its compute passes wrote them.
    compute_barrier(
        cmd,
        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | compute,
        kComputeRw, compute, kComputeRw);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set_, 0, nullptr);
    SkyPush push;
    push.views = std::min(static_cast<std::uint32_t>(views.size()), views_);
    push.adapt = adapt;
    push.key = static_cast<float>(kExposureKey);
    const double sun_luminance = 0.2126 * atmosphere_params_.sun_illuminance[0] +
                                 0.7152 * atmosphere_params_.sun_illuminance[1] +
                                 0.0722 * atmosphere_params_.sun_illuminance[2];
    push.floor_illuminance = static_cast<float>(kExposureFloor * sun_luminance);
    const auto set_view = [&](std::uint32_t v) {
        const SkyViewInput& in = views[v];
        push.view = v;
        push.eye_radius = static_cast<float>(atmosphere_params_.bottom_radius_km +
                                             std::max(in.eye_height_km, 0.0));
        push.mu_s = static_cast<float>(in.sun_mu);
        push.sun_up = static_cast<float>(in.sun_up);
        vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    };
    if (push.views == 0) {
        return;
    }
    if (tables_dirty_) {
        set_view(0);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[0]);
        vkCmdDispatch(cmd, kTransmittanceWidth / 8, kTransmittanceHeight / 8, 1);
        compute_barrier(cmd, compute, kComputeRw, compute, kComputeRw);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[1]);
        vkCmdDispatch(cmd, kMultiScatteringSize, kMultiScatteringSize, 1);
        compute_barrier(cmd, compute, kComputeRw, compute, kComputeRw);
        tables_dirty_ = false;
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[2]);
    for (std::uint32_t v = 0; v < push.views; ++v) {
        set_view(v);
        vkCmdDispatch(cmd, (kSkyViewWidth + 7) / 8, (kSkyViewHeight + 7) / 8, 1);
    }
    compute_barrier(cmd, compute, kComputeRw, compute, kComputeRw);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[3]);
    for (std::uint32_t v = 0; v < push.views; ++v) {
        set_view(v);
        vkCmdDispatch(cmd, 1, 1, 1);
    }
    compute_barrier(cmd, compute, kComputeRw, compute, kComputeRw);
    set_view(0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines_[4]);
    vkCmdDispatch(cmd, 1, 1, 1);
    compute_barrier(cmd, compute, kComputeRw,
                    VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT |
                        VK_ACCESS_2_UNIFORM_READ_BIT);
}

SkyReadback SkyGpu::read_back(std::uint32_t view) {
    check(vkDeviceWaitIdle(gpu_.device), "vkDeviceWaitIdle");
    const VkDeviceSize t_bytes = VkDeviceSize{kTransmittanceWidth} * kTransmittanceHeight * 8;
    const VkDeviceSize m_bytes = VkDeviceSize{kMultiScatteringSize} * kMultiScatteringSize * 8;
    const VkDeviceSize s_bytes = VkDeviceSize{kSkyViewWidth} * kSkyViewHeight * 8;
    Buffer out = make_buffer(t_bytes + m_bytes + s_bytes + sizeof(ViewSky),
                             VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = gpu_.queue_family;
    check(vkCreateCommandPool(gpu_.device, &pci, nullptr, &pool), "vkCreateCommandPool");
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(gpu_.device, &cai, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer");
    compute_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    const auto copy = [&](VkImage image, std::uint32_t w, std::uint32_t h, std::uint32_t layer,
                          VkDeviceSize at) {
        VkBufferImageCopy c{};
        c.bufferOffset = at;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        c.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_GENERAL, out.buffer, 1, &c);
    };
    copy(transmittance_.image, kTransmittanceWidth, kTransmittanceHeight, 0, 0);
    copy(multi_scattering_.image, kMultiScatteringSize, kMultiScatteringSize, 0, t_bytes);
    copy(sky_view_.image, kSkyViewWidth, kSkyViewHeight, std::min(view, views_ - 1),
         t_bytes + m_bytes);
    const VkBufferCopy bc{sizeof(ViewSky) * std::min(view, views_ - 1), t_bytes + m_bytes + s_bytes,
                          sizeof(ViewSky)};
    vkCmdCopyBuffer(cmd, view_sky_.buffer, out.buffer, 1, &bc);
    compute_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    VkCommandBufferSubmitInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    check(vkQueueSubmit2(gpu_.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2");
    check(vkQueueWaitIdle(gpu_.queue), "vkQueueWaitIdle");
    vkDestroyCommandPool(gpu_.device, pool, nullptr);
    (void)vmaInvalidateAllocation(gpu_.allocator, out.memory, 0, VK_WHOLE_SIZE);

    const auto* halves = static_cast<const std::uint16_t*>(out.mapped);
    const auto unpack = [&](VkDeviceSize at, VkDeviceSize bytes) {
        std::vector<float> v(bytes / 2);
        for (std::size_t i = 0; i < v.size(); ++i) {
            v[i] = half_to_float(halves[at / 2 + i]);
        }
        return v;
    };
    SkyReadback r;
    r.transmittance = unpack(0, t_bytes);
    r.multi_scattering = unpack(t_bytes, m_bytes);
    r.sky_view = unpack(t_bytes + m_bytes, s_bytes);
    ViewSky s;
    std::memcpy(&s, static_cast<const std::uint8_t*>(out.mapped) + t_bytes + m_bytes + s_bytes,
                sizeof(s));
    r.sun_irradiance = s.sun_irradiance;
    r.sky_irradiance = s.sky_irradiance;
    r.exposure = s.exposure;
    destroy(out);
    return r;
}

}  // namespace fovea
