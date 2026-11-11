#include "lights.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "fovea/core/geodesy.hpp"
#include "stars.hpp"

namespace fovea {

namespace {

// Mirrors Light in shaders/lights.slang (std430).
struct GpuLight {
    std::array<float, 3> offset{};
    std::uint32_t packed = 0;
    std::array<float, 3> axis{};
    float papi_tan = 0.0F;
    std::array<float, 3> up{};
    float unused = 0.0F;
};
static_assert(sizeof(GpuLight) == 48, "must match Light in lights.slang");

// Mirrors LightPush in shaders/lights.slang.
struct LightPush {
    Mat4 ecef_to_clip{1.0F};
    std::array<float, 3> origin{};
    float pixel_angle = 0.0F;
    float max_size = 1.0F;
};
static_assert(sizeof(LightPush) == 84, "must match LightPush in lights.slang");

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what);
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fail(std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")");
    }
}

std::array<float, 3> floats(const DVec3& v) {
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

}  // namespace

LightsGpu::LightsGpu(const GpuContext& gpu, std::span<const TileLight> lights, VkFormat colour,
                     VkFormat depth, VkShaderModule vertex, VkShaderModule fragment)
    : gpu_(gpu), count_(static_cast<std::uint32_t>(lights.size())) {
    if (lights.empty()) {
        fail("no lights");
    }
    // The origin: their mean, so the farthest is a few kilometres off.
    std::vector<DVec3> ecef;
    ecef.reserve(lights.size());
    for (const TileLight& l : lights) {
        ecef.push_back(geodetic_to_ecef({l.lat_deg, l.lon_deg, l.height_m}));
        origin_ += ecef.back();
    }
    origin_ /= static_cast<double>(lights.size());
    std::vector<GpuLight> gpu_lights(lights.size());
    for (std::size_t i = 0; i < lights.size(); ++i) {
        const TileLight& l = lights[i];
        const DMat3 ned = ned_to_ecef(l.lat_deg, l.lon_deg);
        const double h = glm::radians(static_cast<double>(l.heading_deg));
        GpuLight& g = gpu_lights[i];
        g.offset = floats(ecef[i] - origin_);
        g.packed = static_cast<std::uint32_t>(l.type) |
                   (static_cast<std::uint32_t>(l.colour[0]) << 8U) |
                   (static_cast<std::uint32_t>(l.colour[1]) << 16U);
        g.axis = floats(ned * DVec3(std::cos(h), std::sin(h), 0.0));
        g.papi_tan =
            static_cast<float>(std::tan(glm::radians(static_cast<double>(l.papi_angle_deg))));
        g.up = floats(ned * DVec3(0.0, 0.0, -1.0));
    }

    try {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = sizeof(GpuLight) * gpu_lights.size();
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        check(vmaCreateBuffer(gpu_.allocator, &bci, &aci, &buffer_, &memory_, &info),
              "vmaCreateBuffer (lights)");
        if (info.pMappedData == nullptr) {
            fail("vmaCreateBuffer (lights): not mapped");
        }
        std::memcpy(info.pMappedData, gpu_lights.data(), bci.size);
        (void)vmaFlushAllocation(gpu_.allocator, memory_, 0, VK_WHOLE_SIZE);

        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                   VK_SHADER_STAGE_VERTEX_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{};
        lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        lci.bindingCount = 1;
        lci.pBindings = &binding;
        check(vkCreateDescriptorSetLayout(gpu_.device, &lci, nullptr, &set_layout_),
              "vkCreateDescriptorSetLayout (lights)");
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.maxSets = 1;
        pci.poolSizeCount = 1;
        pci.pPoolSizes = &size;
        check(vkCreateDescriptorPool(gpu_.device, &pci, nullptr, &pool_),
              "vkCreateDescriptorPool (lights)");
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout_;
        check(vkAllocateDescriptorSets(gpu_.device, &ai, &set_),
              "vkAllocateDescriptorSets (lights)");
        const VkDescriptorBufferInfo buffer_info{buffer_, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set_;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &buffer_info;
        vkUpdateDescriptorSets(gpu_.device, 1, &w, 0, nullptr);

        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(LightPush)};
        const std::array<VkDescriptorSetLayout, 2> sets = {set_layout_, gpu_.frame_layout};
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = static_cast<std::uint32_t>(sets.size());
        plci.pSetLayouts = sets.data();
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(gpu_.device, &plci, nullptr, &layout_),
              "vkCreatePipelineLayout (lights)");
        pipeline_ =
            make_point_pipeline(gpu_.device, layout_, vertex, fragment, colour, depth, "lights");
    } catch (...) {
        release();
        throw;
    }
}

LightsGpu::~LightsGpu() {
    release();
}

void LightsGpu::release() {
    if (gpu_.device == VK_NULL_HANDLE) {
        return;
    }
    vkDestroyPipeline(gpu_.device, pipeline_, nullptr);
    vkDestroyPipelineLayout(gpu_.device, layout_, nullptr);
    vkDestroyDescriptorPool(gpu_.device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(gpu_.device, set_layout_, nullptr);
    if (buffer_ != VK_NULL_HANDLE) {
        vmaDestroyBuffer(gpu_.allocator, buffer_, memory_);
    }
    gpu_.device = VK_NULL_HANDLE;
}

void LightsGpu::record(VkCommandBuffer cmd, const SceneView& view, VkExtent2D extent,
                       std::uint32_t lighting_offset) {
    LightPush push;
    push.ecef_to_clip = view.ecef_to_clip;
    push.origin = floats(origin_ - view.eye_ecef);
    push.pixel_angle =
        static_cast<float>(std::sqrt(pixel_solid_angle(view.ecef_to_clip, extent)));
    push.max_size = gpu_.max_point_size;
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
