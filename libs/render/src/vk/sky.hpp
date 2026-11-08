#pragma once

// The sky's tables and each view's light (milestone 6 step 2, D-030,
// D-031): the compute passes of shaders/sky.slang and what they write,
// recorded at the start of each frame, before the views' scene passes.

#include <volk.h>

#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <span>

#include "fovea/core/math.hpp"
#include "fovea/render/atmosphere.hpp"
#include "gpu_context.hpp"

namespace fovea {

// Mirrors ViewSky in shaders/include/frame.slang (std430).
struct ViewSky {
    std::array<float, 3> sun_irradiance{};
    float exposure = 0.0F;
    std::array<float, 3> sky_irradiance{};
    float unused = 0.0F;
    std::array<float, 3> moon_irradiance{};
    float unused1 = 0.0F;
};
static_assert(sizeof(ViewSky) == 48, "must match ViewSky in frame.slang");

// What one view's sky needs this frame.
struct SkyViewInput {
    double eye_height_km = 0.0;  // above the ellipsoid
    double sun_mu = 0.0;         // the sun's zenith cosine at the eye
    double sun_up = 1.0;         // how much of its disc is above the horizon
    Mat4 clip_to_local{1.0F};    // the view's clip space to its level frame (x north, z down)
    Vec3 sun_local{0.0F, 0.0F, -1.0F};
    // The moon (step 4): its illuminance over the sun's times how much of
    // its disc is above the horizon, and its direction in the level frame.
    double moon_scale = 0.0;
    Vec3 moon_local{0.0F, 0.0F, 1.0F};
};

class SkyGpu {
public:
    // The tables for `views` views. The shader modules are the five compute
    // entry points, in the order of shaders/sky.slang.
    SkyGpu(const GpuContext& gpu, std::uint32_t views, const Atmosphere& atmosphere,
           std::span<const VkShaderModule, 6> modules);
    ~SkyGpu();
    SkyGpu(const SkyGpu&) = delete;
    SkyGpu& operator=(const SkyGpu&) = delete;

    // Records the frame's passes: the tables if the atmosphere changed (or
    // on the first frame), each view's sky-view table and light, then the
    // exposure, moved `adapt` (0 to 1) of the way to its target. Ends with
    // a barrier that makes all of it visible to the scene's shaders. No
    // allocation.
    void record(VkCommandBuffer cmd, std::span<const SkyViewInput> views, float adapt);

    // The visibility the air is set for (D-030): a change remakes the
    // tables on the next record().
    void set_visibility(double visibility_m);

    // For the frame set (binding 1 to 4 of frame.slang and the scene's
    // shaders).
    VkBuffer view_sky() const { return view_sky_.buffer; }
    VkDeviceSize view_sky_size() const { return view_sky_size_; }
    VkImageView sky_view_tables() const { return sky_view_.view; }
    VkImageView aerial_volumes() const { return aerial_.view; }
    VkImageView transmittance_table() const { return transmittance_.view; }
    VkSampler sampler() const { return sampler_; }
    VkBuffer atmosphere_buffer() const { return atmosphere_.buffer; }
    static VkDeviceSize atmosphere_size();
    const Atmosphere& atmosphere() const { return atmosphere_params_; }
    std::uint32_t views() const { return views_; }
    // Whether record() has run: the tables exist (in GENERAL).
    bool started() const { return started_; }

    // Waits for the GPU and copies the tables and one view's light out
    // (tests only: allocates, and stalls). Submits to the queue, so call it
    // on the thread that draws, as every other submit is.
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
    Image make_volume(std::uint32_t width, std::uint32_t height, std::uint32_t depth) const;
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
    Image aerial_;  // 3D: kAerialDepth slices a view, stacked
    Buffer atmosphere_;
    Buffer view_sky_;
    Buffer exposure_state_;
    VkDeviceSize view_sky_size_ = 0;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::array<VkPipeline, 6> pipelines_{};
    Atmosphere base_atmosphere_;  // before the visibility's aerosols
    double visibility_m_ = 0.0;
    bool atmosphere_dirty_ = true;  // the uniform buffer is to be written
    bool started_ = false;          // the images are in GENERAL and the state cleared
    bool tables_dirty_ = true;      // the atmosphere changed: remake its tables
};

}  // namespace fovea
