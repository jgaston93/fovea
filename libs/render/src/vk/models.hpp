#pragma once

// Models placed on the terrain (milestone 4 step 3, D-023 to D-026). The
// terrain database's placements name models (models/<name>.glb); the paging
// thread reads each the first time a placement needs it (libs/models),
// stages its vertices, indices, materials and images, and records their
// uploads, as it does tiles'; the render thread takes them in and draws
// what has arrived, never waiting. Meshes share one vertex and one index
// buffer, materials one storage buffer, images one bindless array; each
// view draws all its placements' primitives with one indirect draw, each
// placement's transform relative to the eye made in double on the CPU.
//
// Placements come from the root tiles for now (always resident: KHSV's are
// at level 12, its roots); finer levels, paged with their tiles, are for
// when an area needs them.

#include <volk.h>

#include <vk_mem_alloc.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "fovea/core/math.hpp"
#include "fovea/models/model.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/terrain/tile_file.hpp"
#include "gpu_context.hpp"

namespace fovea {

// Room on the GPU for every model a database places.
inline constexpr VkDeviceSize kModelVertexBytes = VkDeviceSize{64} << 20;  // 2 M vertices
inline constexpr VkDeviceSize kModelIndexBytes = VkDeviceSize{64} << 20;   // 16 M indices
inline constexpr std::uint32_t kMaxModelImages = 1024;
inline constexpr std::uint32_t kMaxModelMaterials = 4096;
inline constexpr std::uint32_t kMaxModelDraws = 8192;  // primitives drawn per view and frame
inline constexpr std::uint32_t kMaxPlacements = 65536;

// A model read and decoded, ready to stage (paging thread).
struct PreparedModel {
    Model model;
};

class ModelGpu {
public:
    // Throws std::runtime_error.
    ModelGpu(const GpuContext& gpu, std::filesystem::path database_dir,
             std::span<const Placement> placements, std::uint32_t slots, std::uint32_t outputs,
             VkShaderModule vertex, VkShaderModule fragment, VkFormat colour, VkFormat depth);
    ~ModelGpu();  // the GPU must be idle, the paging thread stopped
    ModelGpu(const ModelGpu&) = delete;
    ModelGpu& operator=(const ModelGpu&) = delete;

    // The distinct models placed, each to be loaded once.
    std::uint32_t model_count() const { return static_cast<std::uint32_t>(models_.size()); }
    const std::string& model_name(std::uint32_t m) const { return models_[m].name; }

    // Paging thread: reads a model (nothing, with why, if it can't be
    // used), how much staging it takes, and stages it at `at` (that much
    // room), recording its uploads into `cmd` and filling in what the
    // render thread will need. Throws on GPU errors.
    std::optional<PreparedModel> prepare(std::uint32_t m, std::string& problem);
    static VkDeviceSize staged_bytes(const PreparedModel& p);
    void stage_and_record(VkCommandBuffer cmd, VkBuffer staging, std::uint8_t* mapped,
                          VkDeviceSize at, std::uint32_t m, const PreparedModel& p);

    // Render thread: a model staged and handed over (its uploads go with
    // this frame's), or given up on.
    void arrived(std::uint32_t m);
    void failed(std::uint32_t m) { models_[m].failed = true; }

    // Draws one view's placements whose models have arrived, inside the
    // rendering pass. No allocation. Returns the placements drawn.
    std::uint32_t record(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t output,
                         const SceneView& view, const std::array<float, 3>& sun);
    std::uint32_t loaded() const { return loaded_; }

private:
    struct Buffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        void* mapped = nullptr;
    };
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VmaAllocation memory = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };
    struct Primitive {
        std::uint32_t first_index = 0;  // in the shared index buffer
        std::uint32_t index_count = 0;
        std::uint32_t material = 0;  // in the shared materials
    };
    // Written by the paging thread before the hand-over, read by the render
    // thread after it.
    struct Entry {
        std::string name;
        std::int32_t vertex_offset = 0;
        std::vector<Primitive> primitives;
        DVec3 centre{0.0};  // a sphere around it, in its own frame
        double radius_m = 0.0;
        std::vector<std::uint32_t> images;  // its slots in the image array
        bool ready = false;                 // render thread: arrived
        bool failed = false;
    };
    struct Placed {
        std::uint32_t model = 0;
        DVec3 origin_ecef{0.0};  // its ground line's point
        DMat3 rotation{1.0};     // its frame (glTF's) to ECEF
    };

    Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host) const;
    void destroy(Buffer& b) const;
    void destroy(Image& i) const;
    void release();  // every Vulkan object, once

    GpuContext gpu_;
    std::filesystem::path dir_;
    std::uint32_t outputs_ = 1;
    std::vector<Entry> models_;
    std::vector<Placed> placed_;
    std::uint32_t loaded_ = 0;

    // The paging thread's: where the next model's data goes.
    VkDeviceSize vertex_used_ = 0;
    VkDeviceSize index_used_ = 0;
    std::uint32_t images_used_ = 0;
    std::uint32_t materials_used_ = 0;
    std::vector<Image> images_;  // kMaxModelImages, made as models arrive

    Buffer vertices_;
    Buffer indices_;
    Buffer materials_;
    Buffer draws_;     // host-visible: slots x outputs x kMaxModelDraws records
    Buffer commands_;  // likewise, the indirect commands

    std::array<VkSampler, 9> samplers_{};  // by wrap u x 3 + wrap v
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet set_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace fovea
