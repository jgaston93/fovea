#pragma once

// Terrain on the GPU (milestone 3 steps 5 and 6). The roots are loaded up
// front; every other tile is paged: each frame each view picks tiles from
// what's resident (terrain_lod.hpp) and asks for the children it wants,
// including those it will want shortly where the eye is heading; a paging
// thread reads them, makes their images and records their uploads; the
// render thread submits those ahead of its own frame and links the tiles in
// (terrain_cache.hpp). The render thread never waits for the paging thread
// or the disk. Tiles are drawn as instances of one grid, their heights and
// imagery in two bindless arrays indexed by cache slot.

#include <volk.h>

#include <vk_mem_alloc.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fovea/core/time.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/render/terrain_cache.hpp"
#include "fovea/render/terrain_lod.hpp"
#include "fovea/terrain/database.hpp"
#include "gpu_context.hpp"
#include "models.hpp"

namespace fovea {

// Most tiles, and most models, taken in one frame: bounds the render
// thread's work.
inline constexpr std::uint32_t kMaxUploadsPerFrame = 16;
inline constexpr std::uint32_t kMaxModelUploadsPerFrame = 4;
// All of a frame's uploads, tiles and models: what uploads() can hold, and
// what a submit must have room for.
inline constexpr std::uint32_t kMaxUploadsInAFrame = kMaxUploadsPerFrame + kMaxModelUploadsPerFrame;

class Pager;

// The terrain's and the models' shader modules (load's).
struct TerrainShaders {
    VkShaderModule vertex = VK_NULL_HANDLE;
    VkShaderModule fragment = VK_NULL_HANDLE;
    VkShaderModule shadow_vertex = VK_NULL_HANDLE;  // step 5
    ModelShaders models;
};

class TerrainGpu {
public:
    // Opens the paging: loads and uploads the roots (waiting, init time),
    // and starts the paging thread. `slots` x `outputs` regions of draw
    // records, one per frame in flight and output. Throws
    // std::runtime_error.
    static std::unique_ptr<TerrainGpu> load(const GpuContext& gpu,
                                            std::unique_ptr<TerrainDatabase> db,
                                            const TerrainOptions& options, std::uint32_t slots,
                                            std::uint32_t outputs, const TerrainShaders& shaders,
                                            VkFormat colour, VkFormat depth, TerrainLoad& report);
    ~TerrainGpu();  // stops the paging thread; the GPU must be idle
    TerrainGpu(const TerrainGpu&) = delete;
    TerrainGpu& operator=(const TerrainGpu&) = delete;

    // At the start of each frame (numbered from 1), with the newest frame
    // the GPU has finished: frees slots retired before it, and takes in
    // tiles the paging thread has ready. Their uploads must be submitted
    // ahead of this frame's commands (uploads()), and submitted() called
    // with the timeline value that submission signals.
    void begin_frame(std::uint64_t frame, std::uint64_t completed);
    std::span<const VkCommandBufferSubmitInfo> uploads() const {
        return {upload_infos_.data(), upload_count_};
    }
    void submitted(std::uint64_t timeline_value);

    // Picks one view's tiles, writes their records and asks for the tiles
    // it wants: before the view's shadow maps and its scene. No allocation.
    // Returns the number of tiles to draw.
    std::uint32_t prepare(std::uint32_t slot, std::uint32_t output, const SceneView& view,
                          VkExtent2D extent);
    // Draws them, and the models, inside a rendering pass with the colour
    // and depth formats given to load, lit by the view's ViewLighting at
    // lighting_offset in the frame set. No allocation.
    void draw(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t output, const SceneView& view,
              std::uint32_t lighting_offset);
    // Draws the casters into one of the view's shadow maps (step 5), inside
    // its depth-only pass: `to_map` takes points relative to the eye to the
    // map. The terrain casts only if `terrain_casts` (the light is low);
    // the models always. No allocation.
    void draw_shadow(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t output,
                     std::uint32_t cascade, const SceneView& view, const Mat4& to_map,
                     bool terrain_casts);

    // This frame's numbers so far (reset by begin_frame).
    const TerrainStats& stats() const { return stats_; }
    // Why paged tiles couldn't be used, so far (any thread; allocates).
    std::vector<std::string> problems() const;

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

private:
    friend class Pager;

    explicit TerrainGpu(const GpuContext& gpu);
    Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) const;
    void destroy(Buffer& b) const;
    void destroy(Image& i) const;
    void write_descriptors(std::uint32_t slot, VkImageView heights, VkImageView imagery);
    void request_children(const SceneView& view, const TileSelection& sel,
                          double pixels_per_tangent);
    // The selected tiles' records and their one draw.
    void draw_tiles(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t o, const SceneView& view,
                    std::uint32_t drawn, std::uint32_t lighting_offset);

    GpuContext gpu_;
    std::unique_ptr<TerrainDatabase> db_;
    double pixel_error_ = 1.0;
    int max_level_ = 30;
    std::uint32_t samples_ = 0;
    std::uint32_t index_count_ = 0;
    std::uint32_t outputs_ = 0;
    std::size_t estimate_bytes_ = 0;  // a tile's GPU memory, before it's read
    std::uint64_t frame_ = 0;

    std::unique_ptr<TileCache> cache_;
    // Per slot: the images. The paging thread makes and replaces them for
    // the slots it fills (a slot is only ever filled once it's free, so
    // nothing on the GPU uses the old ones); roots' are made at load.
    std::vector<Image> heights_;
    std::vector<Image> imagery_;
    Image fallback_;  // grey, for tiles without imagery

    Buffer controls_;  // per slot: 9 x 9 control points
    Buffer indices_;
    Buffer draws_;  // host-visible: slots x outputs x capacity records

    // Scratch, sized once.
    std::vector<std::uint32_t> stack_;
    std::vector<std::uint32_t> selected_;
    std::vector<std::uint32_t> wants_;
    std::vector<std::uint32_t> split_;
    std::vector<double> want_priority_;
    std::vector<std::uint32_t> want_order_;
    std::array<VkCommandBufferSubmitInfo, kMaxUploadsInAFrame> upload_infos_{};
    std::array<std::uint32_t, kMaxUploadsInAFrame> upload_batches_{};
    std::uint32_t upload_count_ = 0;
    // Per output: the eye last frame and when, for where it's heading.
    std::vector<DVec3> last_eye_;
    std::vector<LocalTime> last_eye_at_;
    std::vector<bool> has_last_eye_;

    TerrainStats stats_;
    std::unique_ptr<ModelGpu> models_;  // with placements (milestone 4 step 3)
    std::unique_ptr<Pager> pager_;      // after models_: stopped before it goes

    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipeline shadow_pipeline_ = VK_NULL_HANDLE;
    std::vector<std::uint32_t> drawn_;  // per output: tiles prepare() picked
};

}  // namespace fovea
