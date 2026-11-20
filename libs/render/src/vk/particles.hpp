#pragma once

// Particle volumes (D-039, D-040): a host's dust, smoke or spray, drawn into
// each view's resolved HDR target after the stars and lights, reading its
// resolved depth (shaders/particles.slang). Each frame slot has its own
// host-visible buffer, packed by the render thread at the frame's start
// (pack_particles): the slot's previous frame has finished by then.

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "fovea/render/particle_pack.hpp"
#include "fovea/render/renderer.hpp"
#include "gpu_context.hpp"

namespace fovea {

class ParticlesGpu {
public:
    // Up to `capacity` particles, `slots` frames in flight and `outputs`
    // views. The pipeline draws into `colour`; the modules are
    // shaders/particles.slang's vs_particles and fs_particles. Throws
    // std::runtime_error.
    ParticlesGpu(const GpuContext& gpu, std::size_t capacity, std::uint32_t slots,
                 std::uint32_t outputs, VkFormat colour, VkShaderModule vertex,
                 VkShaderModule fragment);
    ~ParticlesGpu();
    ParticlesGpu(const ParticlesGpu&) = delete;
    ParticlesGpu& operator=(const ParticlesGpu&) = delete;

    // The resolved depth view `output` reads, in SHADER_READ_ONLY_OPTIMAL
    // when drawn (its target made or remade, with no frame in flight).
    void set_depth(std::uint32_t output, VkImageView depth);

    // Packs a frame into `slot` (pack_particles), relative to `anchor`
    // (ECEF), and returns how many will be drawn. No allocation.
    std::size_t pack(std::uint32_t slot, const ParticleFrame& frame, LocalTime at,
                     const ParticleLook& look, const DVec3& anchor) noexcept;
    void clear(std::uint32_t slot) noexcept { counts_[slot] = 0; }
    std::size_t count(std::uint32_t slot) const { return counts_.at(slot); }

    // Draws `slot`'s particles for one view, inside a rendering pass on its
    // resolved target (no depth attachment), lit by its block of the frame
    // set. No allocation.
    void record(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t output,
                const SceneView& view, std::uint32_t lighting_offset,
                const ParticleLook& look) const;

private:
    struct Slot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        void* mapped = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    void release();

    GpuContext gpu_;
    std::size_t capacity_ = 0;
    std::vector<Slot> slots_;
    std::vector<std::size_t> counts_;
    std::vector<DVec3> anchors_;
    std::vector<VkDescriptorSet> depth_sets_;  // one per output
    VkDescriptorSetLayout buffers_layout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout depth_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace fovea
