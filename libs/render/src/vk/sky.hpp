#pragma once

// The sky's tables and each view's light (milestone 6 step 2, D-030,
// D-031): the compute passes of shaders/sky.slang and what they write,
// recorded at the start of each frame, before the views' scene passes.

#include <volk.h>

#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <span>

#include "fovea/render/atmosphere.hpp"
#include "gpu_context.hpp"

namespace fovea {

// Mirrors ViewSky in shaders/include/frame.slang (std430).
struct ViewSky {
    std::array<float, 3> sun_irradiance{};
    float exposure = 0.0F;
    std::array<float, 3> sky_irradiance{};
    float unused = 0.0F;
};
static_assert(sizeof(ViewSky) == 32, "must match ViewSky in frame.slang");

// What one view's sky needs this frame.
struct SkyViewInput {
    double eye_height_km = 0.0;  // above the ellipsoid
    double sun_mu = 0.0;         // the sun's zenith cosine at the eye
    double sun_up = 1.0;         // how much of its disc is above the horizon
};

class SkyGpu {
public:
    // The tables for `views` views. The shader modules are the five compute
    // entry points, in the order of shaders/sky.slang.
    SkyGpu(const GpuContext& gpu, std::uint32_t views, const Atmosphere& atmosphere,
           std::span<const VkShaderModule, 5> modules);
    ~SkyGpu();
    SkyGpu(const SkyGpu&) = delete;
    SkyGpu& operator=(const SkyGpu&) = delete;

    // Records the frame's passes: the tables if the atmosphere changed (or
    // on the first frame), each view's sky-view table and light, then the
    // exposure, moved `adapt` (0 to 1) of the way to its target. Ends with
    // a barrier that makes all of it visible to the scene's shaders. No
    // allocation.
    void record(VkCommandBuffer cmd, std::span<const SkyViewInput> views, float adapt);

    // For the frame set (binding 1, 2 and 3 of frame.slang and the scene's
    // shaders).
    VkBuffer view_sky() const { return view_sky_.buffer; }
    VkDeviceSize view_sky_size() const { return view_sky_size_; }
    VkImageView sky_view_tables() const { return sky_view_.view; }
    VkSampler sampler() const { return sampler_; }
    VkBuffer atmosphere_buffer() const { return atmosphere_.buffer; }
    static VkDeviceSize atmosphere_size();

    // Waits for the GPU and copies the tables and one view's light out
    // (tests only: allocates, and stalls).
    SkyReadback read_back(std::uint32_t view);

private:
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        void* mapped = nullptr;
    };
    Image make_image(std::uint32_t width, std::uint32_t height, std::uint32_t layers,
                     bool array) const;
    void release();
    Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) const;
    void destroy(Image& i) const;
    void destroy(Buffer& b) const;

    GpuContext gpu_;
    std::uint32_t views_ = 0;
    Atmosphere atmosphere_params_;
    Image transmittance_;
    Image multi_scattering_;
    Image sky_view_;
    Buffer atmosphere_;
    Buffer view_sky_;
    Buffer exposure_state_;
    VkDeviceSize view_sky_size_ = 0;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, 5> pipelines_{};
    bool started_ = false;      // the images are in GENERAL and the state cleared
    bool tables_dirty_ = true;  // the atmosphere changed: remake its tables
};

}  // namespace fovea
