#pragma once

// Particle volumes (D-039, D-040, D-041): a host's dust, smoke or spray,
// drawn into each view's resolved HDR target after the stars and lights,
// reading its resolved depth, shadowed by itself by half-angle slicing
// (shaders/particles.slang). Each frame slot has its own host-visible
// buffers, packed by the render thread at the frame's start (pack): the
// slot's previous frame has finished by then.

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "fovea/render/particle_pack.hpp"
#include "fovea/render/renderer.hpp"
#include "gpu_context.hpp"

namespace fovea {

class ParticlesGpu {
public:
    // Up to `capacity` particles, `slots` frames in flight, one view per
    // extent. Composites into `colour` targets. The modules are
    // shaders/particles.slang's entry points. Throws std::runtime_error.
    struct Modules {
        VkShaderModule vs_particles = VK_NULL_HANDLE;
        VkShaderModule fs_particles = VK_NULL_HANDLE;
        VkShaderModule vs_light = VK_NULL_HANDLE;
        VkShaderModule fs_light = VK_NULL_HANDLE;
        VkShaderModule vs_composite = VK_NULL_HANDLE;
        VkShaderModule fs_composite = VK_NULL_HANDLE;
    };
    ParticlesGpu(const GpuContext& gpu, std::size_t capacity, std::uint32_t slots,
                 std::span<const VkExtent2D> extents, VkFormat colour, const Modules& modules,
                 const ParticleQuality& quality);
    ~ParticlesGpu();
    ParticlesGpu(const ParticlesGpu&) = delete;
    ParticlesGpu& operator=(const ParticlesGpu&) = delete;

    // The resolved depth view `output` reads, in SHADER_READ_ONLY_OPTIMAL
    // when drawn (its target made or remade, with no frame in flight).
    void set_depth(std::uint32_t output, VkImageView depth);

    // Packs a frame into `slot` (pack_particles), relative to `anchor`
    // (ECEF), and sorts it for each view along its half vector with the
    // sun's light (`sun`: toward the sun, ECEF). Returns how many will be
    // drawn. No allocation.
    std::size_t pack(std::uint32_t slot, const ParticleFrame& frame, LocalTime at,
                     const ParticleLook& look, const DVec3& anchor,
                     std::span<const SceneView> views, const DVec3& sun) noexcept;
    void clear(std::uint32_t slot) noexcept { counts_[slot] = 0; }
    std::size_t count(std::uint32_t slot) const { return counts_.at(slot); }

    // Draws `slot`'s particles for view `output` and composites them over
    // `target` (the view's resolved HDR target, in COLOR_ATTACHMENT_OPTIMAL,
    // its writes so far made available to colour attachment access), lit by
    // the view's block of the frame set. Records its own rendering passes;
    // none may be open. No allocation.
    void record(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t output,
                const SceneView& view, std::uint32_t lighting_offset, const ParticleLook& look,
                VkImageView target, VkExtent2D extent) const;

private:
    struct Slot {
        VkBuffer buffer = VK_NULL_HANDLE;  // origins, light space, particles
        VmaAllocation memory = nullptr;
        void* mapped = nullptr;
        VkBuffer order = VK_NULL_HANDLE;  // each view's sorted indices
        VmaAllocation order_memory = nullptr;
        void* order_mapped = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };
    void release();
    Image make_image(VkExtent2D extent, VkFormat format) const;
    void destroy_image(Image& i) const;

    GpuContext gpu_;
    std::size_t capacity_ = 0;
    ParticleQuality quality_;
    std::vector<Slot> slots_;
    std::vector<std::size_t> counts_;
    std::vector<std::size_t> sorted_views_;  // per slot: the outputs pack sorted for
    std::vector<DVec3> anchors_;
    std::vector<std::uint8_t> front_to_back_;  // per slot and output
    std::vector<VkDescriptorSet> view_sets_;   // per output: depth, light, image
    std::vector<Image> particle_images_;       // per output, its render size (or half)
    std::vector<VkExtent2D> image_extents_;
    Image light_;                                  // shared, used by one view at a time
    VkFormat light_format_ = VK_FORMAT_UNDEFINED;  // R32F where it blends and filters, else R16F
    VkSampler light_sampler_ = VK_NULL_HANDLE;
    // Scratch for the sort, sized at construction.
    std::vector<Vec3> centres_;
    std::vector<std::uint32_t> keys_;
    std::vector<std::uint32_t> scratch_;
    VkDescriptorSetLayout buffers_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout view_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline over_ = VK_NULL_HANDLE;   // back to front from the eye
    VkPipeline under_ = VK_NULL_HANDLE;  // front to back from the eye
    VkPipeline light_pipeline_ = VK_NULL_HANDLE;
    VkPipeline composite_ = VK_NULL_HANDLE;
};

}  // namespace fovea
