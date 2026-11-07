#include "terrain.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>

#include "fovea/core/containers.hpp"

#include "fovea/core/geodesy.hpp"
#include "fovea/terrain/tile_file.hpp"
#include "fovea/terrain/tile_key.hpp"

namespace fovea {

namespace {

// The control points per side of a tile (shaders/terrain.slang).
constexpr std::uint32_t kControls = 9;
// Staging memory for uploads, reused: at least a tile's worth.
constexpr VkDeviceSize kStagingBytes = VkDeviceSize{128} << 20;

// shaders/terrain.slang's blocks.
struct Control {
    std::array<float, 4> position{};
    std::array<float, 4> up{};
};
static_assert(sizeof(Control) == 32, "must match Control in terrain.slang");

struct TileDraw {
    std::array<float, 3> offset{};
    std::uint32_t tile = 0;
    float skirt_m = 0.0F;
    std::array<std::uint32_t, 3> unused{};
};
static_assert(sizeof(TileDraw) == 32, "must match TileDraw in terrain.slang");

struct TerrainPush {
    Mat4 ecef_to_clip{1.0F};
    std::array<float, 3> sun{};
    std::uint32_t samples = 0;
};
static_assert(sizeof(TerrainPush) == 80, "must match TerrainPush in terrain.slang");

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what);
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fail(std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")");
    }
}

VkFormat vk_format(ImageryFormat f) {
    switch (f) {
        case ImageryFormat::bc7_srgb:
            return VK_FORMAT_BC7_SRGB_BLOCK;
        case ImageryFormat::bc1_srgb:
            return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
        case ImageryFormat::rgba8_srgb:
            return VK_FORMAT_R8G8B8A8_SRGB;
    }
    return VK_FORMAT_UNDEFINED;
}

void image_barrier(VkCommandBuffer cmd, VkImage image, std::uint32_t mips, VkImageLayout from,
                   VkImageLayout to, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
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
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    VkDependencyInfo d{};
    d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

// Copies data to the GPU through one staging buffer, in batches: each
// batch is submitted and waited for before the buffer is reused (init
// time only).
class Uploader {
public:
    explicit Uploader(const GpuContext& gpu) : gpu_(gpu) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = kStagingBytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        check(vmaCreateBuffer(gpu.allocator, &bci, &aci, &staging_, &memory_, &info),
              "vmaCreateBuffer (staging)");
        mapped_ = static_cast<std::uint8_t*>(info.pMappedData);
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = gpu.queue_family;
        check(vkCreateCommandPool(gpu.device, &pci, nullptr, &pool_), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(gpu.device, &ai, &cmd_), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        check(vkCreateFence(gpu.device, &fci, nullptr, &fence_), "vkCreateFence");
    }
    ~Uploader() {
        (void)vkDeviceWaitIdle(gpu_.device);
        vkDestroyFence(gpu_.device, fence_, nullptr);
        vkDestroyCommandPool(gpu_.device, pool_, nullptr);
        vmaDestroyBuffer(gpu_.allocator, staging_, memory_);
    }
    Uploader(const Uploader&) = delete;
    Uploader& operator=(const Uploader&) = delete;

    // Room for `bytes` more in this batch (flushing the batch if needed).
    void reserve(VkDeviceSize bytes) {
        if (bytes > kStagingBytes) {
            fail("terrain: a tile larger than the staging buffer (" + std::to_string(bytes) +
                 " bytes)");
        }
        if (used_ + bytes > kStagingBytes) {
            flush();
        }
        if (!recording_) {
            check(vkResetCommandBuffer(cmd_, 0), "vkResetCommandBuffer");
            VkCommandBufferBeginInfo begin{};
            begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(cmd_, &begin), "vkBeginCommandBuffer");
            recording_ = true;
        }
    }
    // Copies bytes into the batch (after reserve); returns their offset.
    VkDeviceSize put(const void* data, std::size_t bytes) {
        const VkDeviceSize at = used_;
        std::memcpy(mapped_ + at, data, bytes);
        // 16: BC blocks' size, which copies from a buffer must align to.
        used_ = (used_ + bytes + 15U) & ~VkDeviceSize{15};
        return at;
    }
    VkCommandBuffer cmd() const { return cmd_; }
    VkBuffer staging() const { return staging_; }
    std::uint8_t* mapped() const { return mapped_; }
    // Where the next put goes; skip moves past `bytes` written directly.
    VkDeviceSize offset() const { return used_; }
    void skip(std::size_t bytes) { used_ = (used_ + bytes + 15U) & ~VkDeviceSize{15}; }

    void flush() {
        if (!recording_) {
            return;
        }
        check(vmaFlushAllocation(gpu_.allocator, memory_, 0, used_), "vmaFlushAllocation");
        check(vkEndCommandBuffer(cmd_), "vkEndCommandBuffer");
        VkCommandBufferSubmitInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        cbi.commandBuffer = cmd_;
        VkSubmitInfo2 submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &cbi;
        check(vkQueueSubmit2(gpu_.queue, 1, &submit, fence_), "vkQueueSubmit2");
        check(vkWaitForFences(gpu_.device, 1, &fence_, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        check(vkResetFences(gpu_.device, 1, &fence_), "vkResetFences");
        recording_ = false;
        used_ = 0;
    }

private:
    GpuContext gpu_;
    VkBuffer staging_ = VK_NULL_HANDLE;
    VmaAllocation memory_ = nullptr;
    std::uint8_t* mapped_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkDeviceSize used_ = 0;
    bool recording_ = false;
};

// One image's mips into it, from data already in the batch.
void upload_image(Uploader& up, VkImage image, std::span<const VkDeviceSize> offsets,
                  std::uint32_t size) {
    const auto mips = static_cast<std::uint32_t>(offsets.size());
    image_barrier(up.cmd(), image, mips, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                  VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (std::uint32_t m = 0; m < mips; ++m) {
        const std::uint32_t s = std::max(1U, size >> m);
        VkBufferImageCopy copy{};
        copy.bufferOffset = offsets[m];
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
        copy.imageExtent = {s, s, 1};
        vkCmdCopyBufferToImage(up.cmd(), up.staging(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &copy);
    }
    image_barrier(up.cmd(), image, mips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                  VK_ACCESS_2_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

// The direction towards the sun at the eye: fixed for now, 45 degrees up
// in the south-east.
DVec3 sun_direction(const DVec3& eye_ecef) {
    const Geodetic g = ecef_to_geodetic(eye_ecef);
    const double el = glm::radians(45.0);
    const double az = glm::radians(135.0);
    const DVec3 ned(std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), -std::sin(el));
    return ned_to_ecef(g.lat_deg, g.lon_deg) * ned;
}

DVec3 ellipsoid_up(double lat_deg, double lon_deg) {
    const double lat = glm::radians(lat_deg);
    const double lon = glm::radians(lon_deg);
    return {std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat)};
}

}  // namespace

TerrainGpu::Buffer TerrainGpu::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage,
                                           bool host) const {
    Buffer b;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = usage;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (host) {
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VmaAllocationInfo info{};
    check(vmaCreateBuffer(gpu_.allocator, &bci, &aci, &b.buffer, &b.memory, &info),
          "vmaCreateBuffer");
    b.mapped = info.pMappedData;
    return b;
}

void TerrainGpu::destroy(Buffer& b) const {
    if (b.buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(gpu_.allocator, b.buffer, b.memory);
    }
    b = {};
}

void TerrainGpu::destroy(Image& i) const {
    if (i.view != VK_NULL_HANDLE) {
        vkDestroyImageView(gpu_.device, i.view, nullptr);
    }
    if (i.image != VK_NULL_HANDLE) {
        vmaDestroyImage(gpu_.allocator, i.image, i.memory);
    }
    i = {};
}

namespace {

// A tile read and decoded, ready to stage (paging thread, or load).
struct PreparedTile {
    bool ok = false;
    std::string problem;  // why not ok, or why its imagery was dropped
    TileInfo info;
    std::vector<float> heights;
    bool has_imagery = false;
    ImageryImage imagery;
    std::array<Control, std::size_t{kControls} * kControls> controls{};
    std::vector<Placement> placements;  // in a tile at the placements' level
};

struct PrepareContext {
    std::uint32_t samples = 0;
    int max_level = 30;
    bool bc7 = false;
    bool bc1 = false;
    float max_root_error = 0.0F;  // skirts hang a tile's error and this, and a metre
    int placement_level = -1;     // whose tiles hold placements (none: -1)
};

std::size_t align16(std::size_t n) {
    return (n + 15U) & ~std::size_t{15};
}

// What a prepared tile stages: heights, each mip, the control points.
std::size_t staged_bytes(const PreparedTile& p) {
    std::size_t bytes = align16(p.heights.size() * sizeof(float));
    if (p.has_imagery) {
        for (const auto& mip : p.imagery.mips) {
            bytes += align16(mip.size());
        }
    }
    return bytes + align16(sizeof(p.controls));
}

// The skirt and the sphere around the tile, once the roots' largest error
// is known: a crack between this tile and any drawn neighbour is at most
// both their errors, and the neighbour may be as coarse as a root.
void finish_info(PreparedTile& p, float max_root_error, float min_m, float max_m) {
    p.info.skirt_m = static_cast<float>(p.info.lod.error_m) + max_root_error + 1.0F;
    DVec3 lo(1e300);
    DVec3 hi(-1e300);
    for (const Control& c : p.controls) {
        const DVec3 pos(c.position[0], c.position[1], c.position[2]);
        const DVec3 normal(c.up[0], c.up[1], c.up[2]);
        for (const double h : {static_cast<double>(min_m) - static_cast<double>(p.info.skirt_m),
                               static_cast<double>(max_m)}) {
            const DVec3 q = pos + normal * h;
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
        }
    }
    const DVec3 mid = (lo + hi) * 0.5;
    p.info.lod.centre_ecef = p.info.origin_ecef + mid;
    p.info.lod.radius_m = glm::length(hi - mid) + 1.0;  // and float rounding
}

PreparedTile prepare_tile(const TerrainDatabase& db, const TileKey& key, const PrepareContext& c) {
    PreparedTile p;
    p.info.key = key;
    const TileResult read = db.read(key);
    const TileLayer* hl = read.tile ? read.tile->find(kElevationLayer) : nullptr;
    auto grid = hl != nullptr ? decode_elevation(*hl) : std::nullopt;
    if (!read.tile || !grid || grid->size != c.samples) {
        p.problem = to_string(key) + ": " +
                    (read.tile ? "no usable elevation layer" : "cannot read: " + read.detail);
        return p;
    }
    if (const TileLayer* il = read.tile->find(kImageryLayer)) {
        if (auto decoded = decode_imagery(*il)) {
            p.imagery = std::move(*decoded);
            p.has_imagery = true;
        }
    }
    if (p.has_imagery && ((p.imagery.format == ImageryFormat::bc7_srgb && !c.bc7) ||
                          (p.imagery.format == ImageryFormat::bc1_srgb && !c.bc1))) {
        p.problem = to_string(key) + ": this GPU can't sample " +
                    std::string(imagery_format_name(p.imagery.format));
        p.has_imagery = false;
    }
    if (static_cast<int>(key.level) == c.placement_level) {
        if (const TileLayer* pl = read.tile->find(kPlacementsLayer)) {
            if (auto placed = decode_placements(*pl)) {
                p.placements = std::move(*placed);
            } else {
                p.problem = to_string(key) + ": its placements layer doesn't decode";
            }
        }
    }
    if (p.has_imagery && p.imagery.mips.size() > 16) {
        p.problem = to_string(key) + ": more mips than 16";
        p.has_imagery = false;
    }
    // The control points, in double, relative to the tile's origin on the
    // ellipsoid under its centre.
    const GeoBounds b = tile_bounds(key);
    p.info.origin_ecef =
        geodetic_to_ecef({(b.south_deg + b.north_deg) / 2, (b.west_deg + b.east_deg) / 2, 0.0});
    for (std::uint32_t r = 0; r < kControls; ++r) {
        for (std::uint32_t col = 0; col < kControls; ++col) {
            const double lat = b.south_deg + (b.north_deg - b.south_deg) * r / (kControls - 1);
            const double lon = b.west_deg + (b.east_deg - b.west_deg) * col / (kControls - 1);
            const DVec3 q = geodetic_to_ecef({lat, lon, 0.0}) - p.info.origin_ecef;
            const DVec3 u = ellipsoid_up(lat, lon);
            p.controls[std::size_t{r} * kControls + col] = {
                {static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), 0.0F},
                {static_cast<float>(u.x), static_cast<float>(u.y), static_cast<float>(u.z), 0.0F}};
        }
    }
    p.info.lod.error_m = static_cast<double>(grid->error_m);
    p.info.lod.can_split = key.level < c.max_level && db.manifest().has_tile(children(key)[0]);
    p.heights = std::move(grid->heights);
    p.info.bytes = p.heights.size() * sizeof(float);
    if (p.has_imagery) {
        for (const auto& mip : p.imagery.mips) {
            p.info.bytes += mip.size();
        }
    }
    finish_info(p, c.max_root_error, grid->min_m, grid->max_m);
    p.ok = true;
    return p;
}

TerrainGpu::Image make_image(const GpuContext& gpu, VkFormat format, std::uint32_t size,
                             std::uint32_t mips) {
    // (A plain struct with the same members as TerrainGpu::Image.)
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {size, size, 1};
    ici.mipLevels = mips;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    TerrainGpu::Image img;
    check(vmaCreateImage(gpu.allocator, &ici, &aci, &img.image, &img.memory, nullptr),
          "vmaCreateImage (terrain)");
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = img.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    check(vkCreateImageView(gpu.device, &vci, nullptr, &img.view), "vkCreateImageView");
    return img;
}

// Copies a prepared tile into staging memory at `at` (staged_bytes of
// room), and records its uploads into `cmd`: the heights and imagery into
// their new images, the control points into the slot's place.
void stage_and_record(VkCommandBuffer cmd, VkBuffer staging, std::uint8_t* mapped, VkDeviceSize at,
                      const PreparedTile& p, VkImage heights, VkImage imagery, VkBuffer controls,
                      std::uint32_t slot, std::uint32_t samples) {
    const auto put = [&](const void* data, std::size_t bytes) {
        const VkDeviceSize here = at;
        std::memcpy(mapped + here, data, bytes);
        at += align16(bytes);
        return here;
    };
    const auto upload = [&](VkImage image, std::span<const VkDeviceSize> offsets,
                            std::uint32_t size) {
        const auto mips = static_cast<std::uint32_t>(offsets.size());
        image_barrier(cmd, image, mips, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                      VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        for (std::uint32_t m = 0; m < mips; ++m) {
            const std::uint32_t s = std::max(1U, size >> m);
            VkBufferImageCopy copy{};
            copy.bufferOffset = offsets[m];
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, 1};
            copy.imageExtent = {s, s, 1};
            vkCmdCopyBufferToImage(cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &copy);
        }
        image_barrier(
            cmd, image, mips, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
            VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
            VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    };
    const std::array<VkDeviceSize, 1> h = {put(p.heights.data(), p.heights.size() * sizeof(float))};
    upload(heights, h, samples);
    if (p.has_imagery) {
        std::array<VkDeviceSize, 16> mips{};
        for (std::size_t m = 0; m < p.imagery.mips.size(); ++m) {
            mips[m] = put(p.imagery.mips[m].data(), p.imagery.mips[m].size());
        }
        upload(imagery, std::span(mips.data(), p.imagery.mips.size()), p.imagery.size);
    }
    VkBufferCopy copy{};
    copy.srcOffset = put(p.controls.data(), sizeof(p.controls));
    copy.dstOffset = VkDeviceSize{slot} * sizeof(p.controls);
    copy.size = sizeof(p.controls);
    // After any earlier frame's vertex shaders read this slot's old points.
    VkMemoryBarrier2 before{};
    before.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    before.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo d{};
    d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &before;
    vkCmdPipelineBarrier2(cmd, &d);
    vkCmdCopyBuffer(cmd, staging, controls, 1, &copy);
    VkMemoryBarrier2 after{};
    after.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    after.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    after.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    after.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    d.pMemoryBarriers = &after;
    vkCmdPipelineBarrier2(cmd, &d);
}

VkFormat imagery_vk_format(const PreparedTile& p) {
    return vk_format(p.imagery.format);
}

}  // namespace

// A tile asked for, and one delivered.
struct TileRequest {
    std::uint32_t slot = 0;
    TileKey key;
    double priority = 0.0;
};

struct TileArrival {
    std::uint32_t slot = 0;
    bool ok = false;
    bool retry = false;  // not ok for now (a GPU error), not for good
    TileInfo info;
    VkImageView heights = VK_NULL_HANDLE;
    VkImageView imagery = VK_NULL_HANDLE;  // null: the grey fallback
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::uint32_t batch = 0;
};

// A placed model staged, or given up on.
struct ModelArrival {
    std::uint32_t model = 0;
    bool ok = false;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::uint32_t batch = 0;
};

// The paging thread: takes requests, reads and decodes tiles (most wanted
// first), makes their images and records their uploads into staging memory
// it reuses once the frames that submitted them have finished (the
// timeline).
class Pager {
public:
    Pager(TerrainGpu& t, const PrepareContext& ctx) : t_(t), ctx_(ctx) {
        const GpuContext& gpu = t.gpu_;
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = kStagingBytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                    VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        check(vmaCreateBuffer(gpu.allocator, &bci, &aci, &staging_, &staging_memory_, &info),
              "vmaCreateBuffer (paging staging)");
        mapped_ = static_cast<std::uint8_t*>(info.pMappedData);
        VkCommandPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = gpu.queue_family;
        check(vkCreateCommandPool(gpu.device, &pci, nullptr, &pool_), "vkCreateCommandPool");
        std::array<VkCommandBuffer, kBatches> cmds{};
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = kBatches;
        check(vkAllocateCommandBuffers(gpu.device, &ai, cmds.data()), "vkAllocateCommandBuffers");
        for (std::size_t i = 0; i < kBatches; ++i) {
            batches_[i].cmd = cmds[i];
        }
        thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
    }
    ~Pager() {
        thread_.request_stop();
        thread_.join();
        // The GPU is idle (the renderer waited): everything can go.
        vkDestroyCommandPool(t_.gpu_.device, pool_, nullptr);
        vmaDestroyBuffer(t_.gpu_.allocator, staging_, staging_memory_);
    }
    Pager(const Pager&) = delete;
    Pager& operator=(const Pager&) = delete;

    // Render thread.
    bool request(const TileRequest& r) { return requests_.try_push(r); }
    std::optional<TileArrival> take() { return arrivals_.try_pop(); }
    std::optional<ModelArrival> take_model() { return model_arrivals_.try_pop(); }
    void submitted(std::uint32_t batch, std::uint64_t value) {
        batches_[batch].value.store(value, std::memory_order_release);
    }

    // Any thread: problems met so far (tiles that couldn't be used).
    std::vector<std::string> problems() {
        const std::lock_guard lock(mutex_);
        return problems_;
    }

private:
    static constexpr std::uint32_t kBatches = 64;
    struct Batch {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkDeviceSize begin = 0;
        std::atomic<std::uint64_t> value{
            0};  // the timeline value its submission signals; 0: not yet
        bool busy = false;
    };

    void note(std::string problem) {
        const std::lock_guard lock(mutex_);
        if (problems_.size() < 1000) {
            problems_.push_back(std::move(problem));
        }
    }

    // Frees batches (in staging order) whose frames have finished.
    void reclaim() {
        std::uint64_t done = 0;
        if (vkGetSemaphoreCounterValue(t_.gpu_.device, t_.gpu_.timeline, &done) != VK_SUCCESS) {
            return;
        }
        while (!in_flight_.empty()) {
            Batch& b = batches_[in_flight_.front()];
            const std::uint64_t v = b.value.load(std::memory_order_acquire);
            if (v == 0 || v > done) {
                break;
            }
            b.busy = false;
            b.value.store(0, std::memory_order_relaxed);
            in_flight_.pop_front();
        }
        if (in_flight_.empty()) {
            head_ = 0;
        }
    }

    // Room in the staging ring, or nothing yet.
    std::optional<VkDeviceSize> staging_room(VkDeviceSize size) const {
        if (in_flight_.empty()) {
            return size <= kStagingBytes ? std::optional(VkDeviceSize{0}) : std::nullopt;
        }
        const VkDeviceSize tail = batches_[in_flight_.front()].begin;
        if (head_ >= tail) {
            if (head_ + size <= kStagingBytes) {
                return head_;
            }
            if (size < tail) {
                return VkDeviceSize{0};
            }
        } else if (head_ + size < tail) {
            return head_;
        }
        return std::nullopt;
    }

    void run(const std::stop_token& stop) {
        std::vector<TileRequest> pending;
        const auto idle = [] { std::this_thread::sleep_for(std::chrono::milliseconds(2)); };
        // Every placed model, once (they're few, and wanted from the start:
        // their placements are in the roots), between tiles.
        std::uint32_t next_model = 0;
        const std::uint32_t models = t_.models_ ? t_.models_->model_count() : 0;
        while (!stop.stop_requested()) {
            while (auto r = requests_.try_pop()) {
                pending.push_back(*r);
            }
            reclaim();
            if (next_model < models) {
                if (!load_model(next_model++, stop)) {
                    return;
                }
                continue;
            }
            if (pending.empty()) {
                idle();
                continue;
            }
            const auto best = std::max_element(
                pending.begin(), pending.end(),
                [](const TileRequest& a, const TileRequest& b) { return a.priority < b.priority; });
            const TileRequest req = *best;
            pending.erase(best);
            TileArrival out;
            out.slot = req.slot;
            try {
                PreparedTile p = prepare_tile(*t_.db_, req.key, ctx_);
                if (!p.problem.empty()) {
                    note(p.problem);
                }
                if (p.ok) {
                    deliver(p, req.slot, out, stop);
                }
            } catch (const std::exception& e) {
                // A GPU or memory error, not the tile's data: asked for
                // again later rather than given up on.
                note(to_string(req.key) + ": " + e.what() + " (will be asked for again)");
                out.ok = false;
                out.retry = true;
            }
            while (!arrivals_.try_push(out)) {
                if (stop.stop_requested()) {
                    return;
                }
                idle();
            }
        }
    }

    // Stages a prepared tile into a free batch and records its uploads.
    void deliver(const PreparedTile& p, std::uint32_t slot, TileArrival& out,
                 const std::stop_token& stop) {
        const GpuContext& gpu = t_.gpu_;
        const VkDeviceSize size = staged_bytes(p);
        if (size > kStagingBytes) {
            note(to_string(p.info.key) + ": " + std::to_string(size) +
                 " bytes, more than the staging memory");
            return;  // not ok: failed for good
        }
        const auto room = acquire(size, stop);
        if (!room) {
            return;  // stopping
        }
        const auto [batch, at] = *room;
        // The slot is free: nothing on the GPU uses its old images.
        t_.destroy(t_.heights_[slot]);
        t_.destroy(t_.imagery_[slot]);
        t_.heights_[slot] = make_image(gpu, VK_FORMAT_R32_SFLOAT, ctx_.samples, 1);
        if (p.has_imagery) {
            t_.imagery_[slot] = make_image(gpu, imagery_vk_format(p), p.imagery.size,
                                           static_cast<std::uint32_t>(p.imagery.mips.size()));
        }
        Batch& b = batches_[batch];
        begin(b);
        stage_and_record(b.cmd, staging_, mapped_, at, p, t_.heights_[slot].image,
                         t_.imagery_[slot].image, t_.controls_.buffer, slot, ctx_.samples);
        commit(batch, at, size);
        out.ok = true;
        out.info = p.info;
        out.heights = t_.heights_[slot].view;
        out.imagery = t_.imagery_[slot].view;
        out.cmd = b.cmd;
        out.batch = batch;
    }

    // A free batch and room in the staging ring for `size` bytes, waiting
    // for frames to finish; nothing if stopping.
    std::optional<std::pair<std::uint32_t, VkDeviceSize>> acquire(VkDeviceSize size,
                                                                  const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            std::uint32_t batch = kBatches;
            for (std::uint32_t i = 0; i < kBatches && batch == kBatches; ++i) {
                batch = batches_[i].busy ? kBatches : i;
            }
            const auto at = staging_room(size);
            if (batch != kBatches && at) {
                return std::pair(batch, *at);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            reclaim();
        }
        return std::nullopt;
    }
    static void begin(Batch& b) {
        check(vkResetCommandBuffer(b.cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo info{};
        info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(b.cmd, &info), "vkBeginCommandBuffer");
    }
    void commit(std::uint32_t batch, VkDeviceSize at, VkDeviceSize size) {
        Batch& b = batches_[batch];
        check(vkEndCommandBuffer(b.cmd), "vkEndCommandBuffer");
        check(vmaFlushAllocation(t_.gpu_.allocator, staging_memory_, at, size),
              "vmaFlushAllocation");
        b.busy = true;
        b.begin = at;
        b.value.store(0, std::memory_order_relaxed);
        in_flight_.push_back(batch);
        head_ = at + size;
    }

    // Reads, stages and hands over one placed model. False if stopping.
    bool load_model(std::uint32_t m, const std::stop_token& stop) {
        ModelGpu& models = *t_.models_;
        ModelArrival out;
        out.model = m;
        try {
            std::string problem;
            auto p = models.prepare(m, problem);
            if (!problem.empty()) {
                note(problem);
            }
            if (p) {
                const VkDeviceSize size = ModelGpu::staged_bytes(*p);
                if (size > kStagingBytes) {
                    note("model '" + models.model_name(m) + "': " + std::to_string(size) +
                         " bytes, more than the staging memory");
                } else if (const auto room = acquire(size, stop)) {
                    const auto [batch, at] = *room;
                    begin(batches_[batch]);
                    models.stage_and_record(batches_[batch].cmd, staging_, mapped_, at, m, *p);
                    commit(batch, at, size);
                    out.ok = true;
                    out.cmd = batches_[batch].cmd;
                    out.batch = batch;
                } else {
                    return false;
                }
            }
        } catch (const std::exception& e) {
            note("model '" + models.model_name(m) + "': " + e.what());
            out.ok = false;
        }
        while (!model_arrivals_.try_push(out)) {
            if (stop.stop_requested()) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return true;
    }

    TerrainGpu& t_;
    PrepareContext ctx_;
    SpscRing<TileRequest, 1024> requests_;
    SpscRing<TileArrival, 64> arrivals_;
    SpscRing<ModelArrival, 64> model_arrivals_;
    std::array<Batch, kBatches> batches_{};
    std::deque<std::uint32_t> in_flight_;  // batches, in staging order
    VkDeviceSize head_ = 0;                // where the next staging goes
    VkBuffer staging_ = VK_NULL_HANDLE;
    VmaAllocation staging_memory_ = nullptr;
    std::uint8_t* mapped_ = nullptr;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    std::mutex mutex_;
    std::vector<std::string> problems_;
    std::jthread thread_;  // last: stopped first
};

std::unique_ptr<TerrainGpu> TerrainGpu::load(const GpuContext& gpu,
                                             std::unique_ptr<TerrainDatabase> db,
                                             const TerrainOptions& options, std::uint32_t slots,
                                             std::uint32_t outputs, VkShaderModule vertex,
                                             VkShaderModule fragment, VkShaderModule model_vertex,
                                             VkShaderModule model_fragment, VkFormat colour,
                                             VkFormat depth, TerrainLoad& report) {
    const auto start = std::chrono::steady_clock::now();
    std::unique_ptr<TerrainGpu> t(new TerrainGpu(gpu));
    t->db_ = std::move(db);
    const DatabaseManifest& m = t->db_->manifest();
    t->samples_ = m.elevation_samples;
    t->pixel_error_ = options.pixel_error;
    t->max_level_ = options.max_level;
    t->outputs_ = std::max(outputs, 1U);
    const std::uint32_t n = t->samples_;
    const std::uint32_t capacity = std::max(options.max_tiles, 1U);
    // A tile's GPU memory before it's read: heights, and imagery as the
    // manifest says (BC7 if it doesn't say), with mips.
    {
        const std::uint32_t texels = m.imagery ? m.imagery->texels : 0;
        const ImageryFormat f = m.imagery ? m.imagery->format : ImageryFormat::bc7_srgb;
        std::size_t imagery = 0;
        for (std::uint32_t s = texels; s >= 1; s /= 2) {
            imagery += imagery_mip_bytes(f, s);
        }
        t->estimate_bytes_ = std::size_t{n} * n * 4 + imagery;
    }

    // The GPU must bind capacity images per array (update-after-bind
    // limits: paging fills entries while frames are in flight).
    VkPhysicalDeviceVulkan12Properties p12{};
    p12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &p12;
    vkGetPhysicalDeviceProperties2(gpu.physical, &props);
    if (capacity > p12.maxPerStageDescriptorUpdateAfterBindSampledImages ||
        std::uint64_t{capacity} * 2 > p12.maxDescriptorSetUpdateAfterBindSampledImages) {
        fail("terrain: " + std::to_string(capacity) + " tiles is more than this GPU binds (" +
             std::to_string(p12.maxPerStageDescriptorUpdateAfterBindSampledImages) +
             " images a stage); lower [terrain] max_tiles");
    }
    const auto supported = [&](VkFormat f) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(gpu.physical, f, &fp);
        return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    };
    PrepareContext ctx;
    ctx.samples = n;
    ctx.max_level = options.max_level;
    ctx.bc7 = gpu.bc_textures && supported(VK_FORMAT_BC7_SRGB_BLOCK);
    ctx.bc1 = gpu.bc_textures && supported(VK_FORMAT_BC1_RGB_SRGB_BLOCK);
    if (m.placements) {
        ctx.placement_level = m.placements->level;
    }

    t->cache_ = std::make_unique<TileCache>(capacity, options.budget_bytes);
    t->heights_.resize(capacity);
    t->imagery_.resize(capacity);
    t->controls_ = t->make_buffer(
        VkDeviceSize{capacity} * kControls * kControls * sizeof(Control),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);

    // The roots: the coarsest level's tiles, read now. Skirts need the
    // largest root error, so they're all read first.
    std::vector<PreparedTile> roots;
    std::vector<std::pair<float, float>> root_heights;  // min, max
    const LevelTiles& top = m.levels.front();
    if (top.level > options.max_level) {
        fail("terrain: [terrain] max_level " + std::to_string(options.max_level) +
             " is above the database's coarsest level, " + std::to_string(top.level));
    }
    for (std::size_t i = 0; i < top.count(); ++i) {
        PreparedTile p = prepare_tile(*t->db_, top.at(i), ctx);
        if (!p.problem.empty()) {
            report.problems.push_back(p.problem);
        }
        if (p.ok) {
            ctx.max_root_error =
                std::max(ctx.max_root_error, static_cast<float>(p.info.lod.error_m));
            roots.push_back(std::move(p));
        }
    }
    if (roots.empty()) {
        fail("terrain: no root tile could be loaded" +
             (report.problems.empty() ? std::string() : " (" + report.problems.front() + ")"));
    }
    if (roots.size() > capacity / 2) {
        fail("terrain: " + std::to_string(roots.size()) + " root tiles leave too little of " +
             std::to_string(capacity) + " tiles for paging; raise [terrain] max_tiles");
    }

    Uploader up(gpu);
    t->fallback_ = make_image(gpu, VK_FORMAT_R8G8B8A8_SRGB, 1, 1);
    {
        const std::array<std::uint8_t, 4> grey = {128, 128, 128, 255};
        up.reserve(grey.size());
        const std::array<VkDeviceSize, 1> at = {up.put(grey.data(), grey.size())};
        upload_image(up, t->fallback_.image, at, 1);
    }
    std::vector<std::pair<std::uint32_t, std::uint32_t>> root_slots;  // slot, index in roots
    for (std::size_t i = 0; i < roots.size(); ++i) {
        PreparedTile& p = roots[i];
        // With the final max_root_error: the skirts and spheres again.
        const auto [lo, hi] = std::minmax_element(p.heights.begin(), p.heights.end());
        finish_info(p, ctx.max_root_error, *lo, *hi);
        const auto slot = t->cache_->add_root(p.info);
        const VkDeviceSize size = staged_bytes(p);
        up.reserve(size);
        const VkDeviceSize at = up.offset();  // where this tile's data starts
        t->heights_[*slot] = make_image(gpu, VK_FORMAT_R32_SFLOAT, n, 1);
        if (p.has_imagery) {
            t->imagery_[*slot] = make_image(gpu, imagery_vk_format(p), p.imagery.size,
                                            static_cast<std::uint32_t>(p.imagery.mips.size()));
        }
        stage_and_record(up.cmd(), up.staging(), up.mapped(), at, p, t->heights_[*slot].image,
                         t->imagery_[*slot].image, t->controls_.buffer, *slot, n);
        up.skip(size);
        root_slots.emplace_back(*slot, static_cast<std::uint32_t>(i));
        report.bytes += p.info.bytes;
    }

    // The shared grid: (n + 2)^2 vertices, the outer ring the skirt.
    const std::uint32_t side = n + 2;
    std::vector<std::uint32_t> indices;
    indices.reserve(std::size_t{side - 1} * (side - 1) * 6);
    for (std::uint32_t r = 0; r + 1 < side; ++r) {
        for (std::uint32_t c = 0; c + 1 < side; ++c) {
            const std::uint32_t a = r * side + c;
            indices.insert(indices.end(), {a, a + 1, a + side, a + 1, a + side + 1, a + side});
        }
    }
    t->index_count_ = static_cast<std::uint32_t>(indices.size());
    t->indices_ =
        t->make_buffer(indices.size() * sizeof(std::uint32_t),
                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    {
        const auto bytes = indices.size() * sizeof(std::uint32_t);
        up.reserve(bytes);
        VkBufferCopy copy{};
        copy.srcOffset = up.put(indices.data(), bytes);
        copy.size = bytes;
        vkCmdCopyBuffer(up.cmd(), up.staging(), t->indices_.buffer, 1, &copy);
        VkMemoryBarrier2 mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
        mb.dstAccessMask = VK_ACCESS_2_INDEX_READ_BIT;
        VkDependencyInfo d{};
        d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        d.memoryBarrierCount = 1;
        d.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(up.cmd(), &d);
    }
    up.flush();

    t->draws_ = t->make_buffer(VkDeviceSize{slots} * t->outputs_ * capacity * sizeof(TileDraw),
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    t->stack_.resize(capacity);
    t->selected_.resize(capacity);
    t->wants_.resize(capacity);
    t->split_.resize(capacity);
    t->want_priority_.resize(capacity);
    t->want_order_.resize(capacity);
    t->last_eye_.resize(t->outputs_);
    t->last_eye_at_.resize(t->outputs_);
    t->has_last_eye_.assign(t->outputs_, false);

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    if (gpu.max_anisotropy > 1.0F) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = std::min(gpu.max_anisotropy, 16.0F);
    }
    check(vkCreateSampler(gpu.device, &sci, nullptr, &t->sampler_), "vkCreateSampler");

    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity, VK_SHADER_STAGE_VERTEX_BIT,
                   nullptr};
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity, VK_SHADER_STAGE_FRAGMENT_BIT,
                   nullptr};
    // The image arrays update after bind (filled as tiles arrive, while
    // frames using the set are in flight) and are partly bound.
    constexpr VkDescriptorBindingFlags kArray =
        VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    const std::array<VkDescriptorBindingFlags, 5> flags = {0, 0, 0, kArray, kArray};
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{};
    flags_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
    flags_info.bindingCount = static_cast<std::uint32_t>(flags.size());
    flags_info.pBindingFlags = flags.data();
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.pNext = &flags_info;
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    lci.bindingCount = static_cast<std::uint32_t>(bindings.size());
    lci.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(gpu.device, &lci, nullptr, &t->set_layout_),
          "vkCreateDescriptorSetLayout");
    const std::array<VkDescriptorPoolSize, 3> sizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, capacity * 2}};
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pci.maxSets = 1;
    pci.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    pci.pPoolSizes = sizes.data();
    check(vkCreateDescriptorPool(gpu.device, &pci, nullptr, &t->pool_), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = t->pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &t->set_layout_;
    check(vkAllocateDescriptorSets(gpu.device, &ai, &t->set_), "vkAllocateDescriptorSets");
    const VkDescriptorBufferInfo controls_info{t->controls_.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorBufferInfo draws_info{t->draws_.buffer, 0, VK_WHOLE_SIZE};
    const VkDescriptorImageInfo sampler_info{t->sampler_, VK_NULL_HANDLE,
                                             VK_IMAGE_LAYOUT_UNDEFINED};
    std::array<VkWriteDescriptorSet, 3> writes{};
    for (std::uint32_t w = 0; w < writes.size(); ++w) {
        writes[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[w].dstSet = t->set_;
        writes[w].dstBinding = w;
        writes[w].descriptorCount = 1;
        writes[w].descriptorType = bindings[w].descriptorType;
    }
    writes[0].pBufferInfo = &controls_info;
    writes[1].pBufferInfo = &draws_info;
    writes[2].pImageInfo = &sampler_info;
    vkUpdateDescriptorSets(gpu.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
    for (const auto& [slot, i] : root_slots) {
        t->write_descriptors(slot, t->heights_[slot].view, t->imagery_[slot].view);
    }

    // The pipeline: the HDR target and a reverse-Z depth buffer (cleared to
    // 0, nearer is greater).
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(TerrainPush)};
    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &t->set_layout_;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &push;
    check(vkCreatePipelineLayout(gpu.device, &plci, nullptr, &t->layout_),
          "vkCreatePipelineLayout");
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
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
    raster.cullMode = VK_CULL_MODE_NONE;  // skirts face either way
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth_state{};
    depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_state.depthTestEnable = VK_TRUE;
    depth_state.depthWriteEnable = VK_TRUE;
    depth_state.depthCompareOp = VK_COMPARE_OP_GREATER;
    VkPipelineColorBlendAttachmentState blend_attachment{};
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blend_attachment;
    const std::array<VkDynamicState, 2> dynamic_states{VK_DYNAMIC_STATE_VIEWPORT,
                                                       VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();
    VkPipelineRenderingCreateInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &colour;
    rendering.depthAttachmentFormat = depth;
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
    ci.layout = t->layout_;
    check(vkCreateGraphicsPipelines(gpu.device, VK_NULL_HANDLE, 1, &ci, nullptr, &t->pipeline_),
          "vkCreateGraphicsPipelines (terrain)");

    // Placements (milestone 4 step 3): from the roots, always resident.
    // Finer levels would page them with their tiles; not yet needed.
    std::vector<Placement> placements;
    if (m.placements && m.placements->level != top.level) {
        report.problems.push_back("placements at level " + std::to_string(m.placements->level) +
                                  ", not the roots' (" + std::to_string(top.level) +
                                  "): not drawn yet");
    }
    for (PreparedTile& p : roots) {
        for (Placement& pl : p.placements) {
            placements.push_back(std::move(pl));
        }
    }
    if (!placements.empty()) {
        report.placements = placements.size();
        t->models_ = std::make_unique<ModelGpu>(gpu, t->db_->dir(), placements, slots, t->outputs_,
                                                model_vertex, model_fragment, colour, depth);
        report.models = t->models_->model_count();
    }

    report.tiles = roots.size();
    report.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    t->pager_ = std::make_unique<Pager>(*t, ctx);
    return t;
}

TerrainGpu::TerrainGpu(const GpuContext& gpu) : gpu_(gpu) {}

TerrainGpu::~TerrainGpu() {
    pager_.reset();  // stops the paging thread first
    vkDestroyPipeline(gpu_.device, pipeline_, nullptr);
    vkDestroyPipelineLayout(gpu_.device, layout_, nullptr);
    vkDestroyDescriptorPool(gpu_.device, pool_, nullptr);
    vkDestroyDescriptorSetLayout(gpu_.device, set_layout_, nullptr);
    vkDestroySampler(gpu_.device, sampler_, nullptr);
    for (Image& i : heights_) {
        destroy(i);
    }
    for (Image& i : imagery_) {
        destroy(i);
    }
    destroy(fallback_);
    destroy(controls_);
    destroy(indices_);
    destroy(draws_);
}

std::vector<std::string> TerrainGpu::problems() const {
    return pager_->problems();
}

void TerrainGpu::write_descriptors(std::uint32_t slot, VkImageView heights, VkImageView imagery) {
    const VkDescriptorImageInfo h{VK_NULL_HANDLE, heights,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo i{VK_NULL_HANDLE,
                                  imagery != VK_NULL_HANDLE ? imagery : fallback_.view,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::uint32_t w = 0; w < writes.size(); ++w) {
        writes[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[w].dstSet = set_;
        writes[w].dstBinding = 3 + w;
        writes[w].dstArrayElement = slot;
        writes[w].descriptorCount = 1;
        writes[w].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    }
    writes[0].pImageInfo = &h;
    writes[1].pImageInfo = &i;
    vkUpdateDescriptorSets(gpu_.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
}

void TerrainGpu::begin_frame(std::uint64_t frame, std::uint64_t completed) {
    frame_ = frame;
    cache_->frames_completed(completed);
    stats_ = {};
    upload_count_ = 0;
    while (upload_count_ < kMaxUploadsPerFrame) {
        const auto a = pager_->take();
        if (!a) {
            break;
        }
        if (!a->ok) {
            if (a->retry) {
                cache_->cancel(a->slot);
            } else {
                cache_->failed_to_load(a->slot);
            }
            continue;
        }
        write_descriptors(a->slot, a->heights, a->imagery);
        cache_->arrived(a->slot, a->info, frame);
        upload_infos_[upload_count_] = {};
        upload_infos_[upload_count_].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        upload_infos_[upload_count_].commandBuffer = a->cmd;
        upload_batches_[upload_count_] = a->batch;
        ++upload_count_;
    }
    stats_.uploads = upload_count_;
    for (std::uint32_t taken = 0; models_ && taken < kMaxModelUploadsPerFrame; ++taken) {
        const auto a = pager_->take_model();
        if (!a) {
            break;
        }
        if (!a->ok) {
            models_->failed(a->model);
            continue;
        }
        models_->arrived(a->model);
        upload_infos_[upload_count_] = {};
        upload_infos_[upload_count_].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
        upload_infos_[upload_count_].commandBuffer = a->cmd;
        upload_batches_[upload_count_] = a->batch;
        ++upload_count_;
    }
    stats_.models_loaded = models_ ? models_->loaded() : 0;
}

void TerrainGpu::submitted(std::uint64_t timeline_value) {
    for (std::uint32_t i = 0; i < upload_count_; ++i) {
        pager_->submitted(upload_batches_[i], timeline_value);
    }
}

void TerrainGpu::request_children(const SceneView& view, const TileSelection& sel,
                                  double pixels_per_tangent) {
    // Most wanted first: by the error each would show now.
    const auto wanted = sel.wanted;
    for (std::size_t i = 0; i < wanted; ++i) {
        const LodTile& tile = cache_->lod()[wants_[i]];
        const double d = glm::length(tile.centre_ecef - view.eye_ecef) - tile.radius_m;
        want_priority_[i] = screen_error_px(tile.error_m, d, pixels_per_tangent);
        want_order_[i] = static_cast<std::uint32_t>(i);
    }
    // At most this many asked for per view per frame, most wanted first
    // (tiles whose children are all asked for already are passed over);
    // later frames ask for the rest.
    constexpr std::size_t kMaxRequests = 16;
    const auto order_end = want_order_.begin() + static_cast<std::ptrdiff_t>(wanted);
    std::sort(want_order_.begin(), order_end, [&](std::uint32_t a, std::uint32_t b) {
        return want_priority_[a] > want_priority_[b];
    });
    std::uint32_t asked = 0;
    const DatabaseManifest& m = db_->manifest();
    for (auto it = want_order_.begin(); it != order_end && asked < kMaxRequests; ++it) {
        const std::uint32_t slot = wants_[*it];
        for (const TileKey& child : children(cache_->info(slot).key)) {
            if (child.level > max_level_ || !m.has_tile(child) || cache_->find(child) ||
                cache_->failed(child)) {
                continue;
            }
            const auto reserved = cache_->reserve(child, estimate_bytes_, frame_);
            if (!reserved) {
                return;  // no room now
            }
            if (!pager_->request({*reserved, child, want_priority_[*it]})) {
                cache_->cancel(*reserved);
                return;
            }
            ++asked;
        }
    }
    stats_.requests += asked;
}

std::uint32_t TerrainGpu::record(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t output,
                                 const SceneView& view, VkExtent2D extent) {
    const std::uint32_t capacity = cache_->capacity();
    LodView lv;
    lv.eye_ecef = view.eye_ecef;
    lv.ecef_to_clip = view.ecef_to_clip;
    lv.pixels_per_tangent =
        static_cast<double>(extent.height) / std::max(static_cast<double>(view.tan_height), 1e-6);
    lv.pixel_error = pixel_error_;
    // Children are asked for a little before they're needed (half the
    // error), and for where the eye will be in a second at its speed now.
    lv.request_error = pixel_error_ * 0.5;
    const LocalTime at = now();
    const std::uint32_t o = std::min(output, outputs_ - 1);
    if (has_last_eye_[o]) {
        const double dt = std::chrono::duration<double>(at - last_eye_at_[o]).count();
        if (dt > 1e-3 && dt < 1.0) {
            DVec3 velocity = (view.eye_ecef - last_eye_[o]) / dt;
            const double speed = glm::length(velocity);
            if (speed > 1000.0) {
                velocity *= 1000.0 / speed;  // a jump, not motion
            }
            lv.lookahead_ecef = view.eye_ecef + velocity * 1.0;
        }
    }
    last_eye_[o] = view.eye_ecef;
    last_eye_at_[o] = at;
    has_last_eye_[o] = true;

    const TileSelection sel =
        select_tiles(cache_->lod(), cache_->roots(), lv, stack_, selected_, wants_, split_);
    for (std::size_t i = 0; i < sel.drawn; ++i) {
        cache_->touch(selected_[i], frame_);
    }
    // A split needs its four children, even those out of view (the split
    // tile's sphere can reach into the view while all its children's
    // don't): kept, or evicting one would undo the split.
    for (std::size_t i = 0; i < sel.split; ++i) {
        cache_->touch(cache_->lod()[split_[i]].children[0], frame_);
    }
    // Children already there of a tile waiting for the rest aren't drawn
    // yet, but are wanted: kept, or evicting them would undo the wait.
    for (std::size_t i = 0; i < sel.wanted; ++i) {
        for (const TileKey& child : children(cache_->info(wants_[i]).key)) {
            if (const auto c = cache_->find(child); c && cache_->resident(*c)) {
                cache_->touch(*c, frame_);
            }
        }
    }
    request_children(view, sel, lv.pixels_per_tangent);
    const TileCacheStats& cs = cache_->stats();
    stats_.drawn += static_cast<std::uint32_t>(sel.drawn);
    stats_.misses += static_cast<std::uint32_t>(sel.misses);
    stats_.resident = cs.resident;
    stats_.loading = cs.loading;
    stats_.bytes = cs.bytes;
    stats_.evictions = cs.evictions;
    stats_.failures = cs.failures;

    const auto drawn = static_cast<std::uint32_t>(sel.drawn);
    const DVec3 sun = sun_direction(view.eye_ecef);
    const std::array<float, 3> sun_f = {static_cast<float>(sun.x), static_cast<float>(sun.y),
                                        static_cast<float>(sun.z)};
    if (drawn == 0) {
        if (models_) {
            stats_.placements_drawn += models_->record(cmd, slot, o, view, sun_f);
        }
        return 0;
    }
    const std::uint32_t base = (slot * outputs_ + o) * capacity;
    auto* records = static_cast<TileDraw*>(draws_.mapped) + base;
    for (std::uint32_t i = 0; i < drawn; ++i) {
        const std::uint32_t t = selected_[i];
        const TileInfo& info = cache_->info(t);
        const DVec3 off = info.origin_ecef - view.eye_ecef;
        records[i] = {
            {static_cast<float>(off.x), static_cast<float>(off.y), static_cast<float>(off.z)},
            t,
            info.skirt_m,
            {}};
    }
    (void)vmaFlushAllocation(gpu_.allocator, draws_.memory, base * sizeof(TileDraw),
                             drawn * sizeof(TileDraw));

    TerrainPush push;
    push.ecef_to_clip = view.ecef_to_clip;
    push.sun = sun_f;
    push.samples = samples_;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    vkCmdBindIndexBuffer(cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    // The records' region: first instance is where this view's start.
    vkCmdDrawIndexed(cmd, index_count_, drawn, 0, 0, base);
    if (models_) {
        stats_.placements_drawn += models_->record(cmd, slot, o, view, sun_f);
    }
    return drawn;
}

}  // namespace fovea
