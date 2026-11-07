#pragma once

// Terrain on the GPU (milestone 3 step 5): a database's tiles uploaded up
// front (heights and imagery as images in two bindless arrays, plus each
// tile's control points), and each frame the tiles each view picks
// (terrain_lod.hpp) drawn as instances of one grid.

#include <volk.h>

#include <vk_mem_alloc.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "fovea/render/renderer.hpp"
#include "fovea/render/terrain_lod.hpp"
#include "fovea/terrain/database.hpp"

namespace fovea {

struct GpuContext {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = nullptr;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    bool bc_textures = false;     // textureCompressionBC enabled
    float max_anisotropy = 0.0F;  // 0: samplerAnisotropy not enabled
};

class TerrainGpu {
public:
    // Reads and uploads every tile down to options.max_level, waiting for
    // the uploads (init time). `slots` x `outputs` regions of draw records,
    // one per frame in flight and output. Throws std::runtime_error.
    static std::unique_ptr<TerrainGpu> load(const GpuContext& gpu, const TerrainDatabase& db,
                                            const TerrainOptions& options, std::uint32_t slots,
                                            std::uint32_t outputs, VkShaderModule vertex,
                                            VkShaderModule fragment, VkFormat colour,
                                            VkFormat depth, TerrainLoad& report);
    ~TerrainGpu();
    TerrainGpu(const TerrainGpu&) = delete;
    TerrainGpu& operator=(const TerrainGpu&) = delete;

    // Picks and draws one view's tiles, inside a rendering pass with the
    // colour and depth formats given to load. No allocation. Returns the
    // number of tiles drawn.
    std::uint32_t record(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t output,
                         const SceneView& view, VkExtent2D extent);

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

    explicit TerrainGpu(const GpuContext& gpu) : gpu_(gpu) {}
    Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) const;
    void destroy(Buffer& b) const;
    void destroy(Image& i) const;

    GpuContext gpu_;
    double pixel_error_ = 1.0;
    std::uint32_t samples_ = 0;
    std::uint32_t index_count_ = 0;
    std::uint32_t outputs_ = 0;

    // Per tile, by index.
    std::vector<LodTile> lod_;
    std::vector<DVec3> origin_ecef_;  // the controls' origin
    std::vector<float> skirt_m_;
    std::vector<std::uint32_t> roots_;
    std::vector<Image> heights_;
    std::vector<Image> imagery_;  // empty images for tiles using the fallback
    Image fallback_;              // grey, for tiles without imagery

    Buffer controls_;
    Buffer indices_;
    Buffer draws_;  // host-visible: slots x outputs x tiles records

    // Scratch for select_tiles, sized once.
    std::vector<std::uint32_t> stack_;
    std::vector<std::uint32_t> selected_;

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace fovea
