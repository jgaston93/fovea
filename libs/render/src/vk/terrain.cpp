#include "terrain.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>

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

std::unique_ptr<TerrainGpu> TerrainGpu::load(const GpuContext& gpu, const TerrainDatabase& db,
                                             const TerrainOptions& options, std::uint32_t slots,
                                             std::uint32_t outputs, VkShaderModule vertex,
                                             VkShaderModule fragment, VkFormat colour,
                                             VkFormat depth, TerrainLoad& report) {
    const auto start = std::chrono::steady_clock::now();
    std::unique_ptr<TerrainGpu> t(new TerrainGpu(gpu));
    const DatabaseManifest& m = db.manifest();
    t->samples_ = m.elevation_samples;
    t->pixel_error_ = options.pixel_error;
    t->outputs_ = std::max(outputs, 1U);
    const std::uint32_t n = t->samples_;

    const auto make_image = [&](VkFormat format, std::uint32_t size, std::uint32_t mips) {
        Image img;
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
    };
    const auto supported = [&](VkFormat f) {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(gpu.physical, f, &p);
        return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    };
    const bool bc7 = gpu.bc_textures && supported(VK_FORMAT_BC7_SRGB_BLOCK);
    const bool bc1 = gpu.bc_textures && supported(VK_FORMAT_BC1_RGB_SRGB_BLOCK);

    Uploader up(gpu);
    {
        // Grey where a tile has no imagery the GPU can use.
        t->fallback_ = make_image(VK_FORMAT_R8G8B8A8_SRGB, 1, 1);
        const std::array<std::uint8_t, 4> grey = {128, 128, 128, 255};
        up.reserve(grey.size());
        const std::array<VkDeviceSize, 1> at = {up.put(grey.data(), grey.size())};
        upload_image(up, t->fallback_.image, at, 1);
    }

    // How many tiles there can be, against what the GPU binds, before
    // reading any: the arrays are update-after-bind (paging will add to
    // them), so those limits apply.
    std::size_t planned = 0;
    for (const LevelTiles& level : m.levels) {
        planned += level.level <= options.max_level ? level.count() : 0;
    }
    VkPhysicalDeviceVulkan12Properties p12{};
    p12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &p12;
    vkGetPhysicalDeviceProperties2(gpu.physical, &props);
    if (planned > p12.maxPerStageDescriptorUpdateAfterBindSampledImages ||
        planned * 2 > p12.maxDescriptorSetUpdateAfterBindSampledImages) {
        fail("terrain: " + std::to_string(planned) + " tiles is more than this GPU binds (" +
             std::to_string(p12.maxPerStageDescriptorUpdateAfterBindSampledImages) +
             " images a stage); lower [terrain] max_level");
    }

    // Every tile down to max_level, coarsest first.
    struct Loaded {
        TileKey key;
        float min_m = 0.0F;
        float max_m = 0.0F;
        float error_m = 0.0F;
    };
    std::vector<Loaded> loaded;
    std::vector<Control> controls;
    std::unordered_map<TileKey, std::uint32_t> index;
    std::size_t no_imagery = 0;
    for (const LevelTiles& level : m.levels) {
        if (level.level > options.max_level) {
            continue;
        }
        for (std::size_t i = 0; i < level.count(); ++i) {
            const TileKey key = level.at(i);
            const TileResult read = db.read(key);
            const TileLayer* hl = read.tile ? read.tile->find(kElevationLayer) : nullptr;
            const auto grid = hl != nullptr ? decode_elevation(*hl) : std::nullopt;
            if (!grid || grid->size != n) {
                report.problems.push_back(
                    to_string(key) + ": " +
                    (read.tile ? "no usable elevation layer" : "cannot read: " + read.detail));
                continue;
            }
            const TileLayer* il = read.tile->find(kImageryLayer);
            auto imagery = il != nullptr ? decode_imagery(*il) : std::nullopt;
            if (imagery && ((imagery->format == ImageryFormat::bc7_srgb && !bc7) ||
                            (imagery->format == ImageryFormat::bc1_srgb && !bc1))) {
                report.problems.push_back(to_string(key) + ": this GPU can't sample " +
                                          std::string(imagery_format_name(imagery->format)));
                imagery.reset();
            }
            if (imagery && imagery->mips.size() > 16) {
                report.problems.push_back(to_string(key) + ": more mips than 16");
                imagery.reset();
            }
            if (!imagery) {
                ++no_imagery;
            }

            std::size_t bytes = std::size_t{n} * n * 4;
            if (imagery) {
                for (const auto& mip : imagery->mips) {
                    bytes += (mip.size() + 15) & ~std::size_t{15};
                }
            }
            up.reserve(bytes + 16);
            Image h = make_image(VK_FORMAT_R32_SFLOAT, n, 1);
            const std::array<VkDeviceSize, 1> hat = {
                up.put(grid->heights.data(), grid->heights.size() * sizeof(float))};
            upload_image(up, h.image, hat, n);
            t->heights_.push_back(h);
            Image im;
            if (imagery) {
                const auto mips = static_cast<std::uint32_t>(imagery->mips.size());
                im = make_image(vk_format(imagery->format), imagery->size, mips);
                std::array<VkDeviceSize, 16> at{};
                for (std::uint32_t k = 0; k < mips; ++k) {
                    at[k] = up.put(imagery->mips[k].data(), imagery->mips[k].size());
                }
                upload_image(up, im.image, std::span(at.data(), mips), imagery->size);
            }
            t->imagery_.push_back(im);
            report.bytes += bytes;

            // The control points, in double, relative to the tile's origin
            // on the ellipsoid under its centre.
            const GeoBounds b = tile_bounds(key);
            const DVec3 origin = geodetic_to_ecef(
                {(b.south_deg + b.north_deg) / 2, (b.west_deg + b.east_deg) / 2, 0.0});
            for (std::uint32_t r = 0; r < kControls; ++r) {
                for (std::uint32_t c = 0; c < kControls; ++c) {
                    const double lat =
                        b.south_deg + (b.north_deg - b.south_deg) * r / (kControls - 1);
                    const double lon = b.west_deg + (b.east_deg - b.west_deg) * c / (kControls - 1);
                    const DVec3 p = geodetic_to_ecef({lat, lon, 0.0}) - origin;
                    const DVec3 u = ellipsoid_up(lat, lon);
                    controls.push_back({{static_cast<float>(p.x), static_cast<float>(p.y),
                                         static_cast<float>(p.z), 0.0F},
                                        {static_cast<float>(u.x), static_cast<float>(u.y),
                                         static_cast<float>(u.z), 0.0F}});
                }
            }
            index.emplace(key, static_cast<std::uint32_t>(loaded.size()));
            if (options.progress) {
                options.progress(loaded.size() + 1, planned);
            }
            t->origin_ecef_.push_back(origin);
            loaded.push_back({key, grid->min_m, grid->max_m, grid->error_m});
        }
    }
    if (loaded.empty()) {
        fail("terrain: no tile could be loaded" +
             (report.problems.empty() ? std::string() : " (" + report.problems.front() + ")"));
    }
    if (no_imagery > 0) {
        report.problems.push_back(std::to_string(no_imagery) +
                                  " tiles without usable imagery are drawn grey");
    }

    // The tree: children where loaded.
    const auto count = static_cast<std::uint32_t>(loaded.size());
    float max_root_error = 0.0F;
    for (const Loaded& l : loaded) {
        if (l.key.level == loaded.front().key.level) {
            max_root_error = std::max(max_root_error, l.error_m);
        }
    }
    t->lod_.resize(count);
    t->skirt_m_.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const Loaded& l = loaded[i];
        LodTile& tile = t->lod_[i];
        tile.error_m = static_cast<double>(l.error_m);
        const auto kids = children(l.key);
        for (std::size_t k = 0; k < kids.size(); ++k) {
            const auto it = index.find(kids[k]);
            tile.children[k] = it != index.end() ? it->second : kNoTile;
        }
        // A root where no ancestor is loaded. Below a missing tile, its
        // loaded descendants are unreachable: the nearest loaded ancestor
        // can't split, and covers them.
        bool covered = false;
        for (auto a = parent(l.key); a && !covered; a = parent(*a)) {
            covered = index.contains(*a);
        }
        if (!covered) {
            t->roots_.push_back(i);
        }
        // A crack between this tile and any drawn neighbour is at most both
        // their errors; the neighbour may be as coarse as a root, so each
        // skirt hangs its own error and the largest root's, and a metre more.
        t->skirt_m_[i] = l.error_m + max_root_error + 1.0F;
        // A sphere around the tile, skirt included: around the control
        // points at its lowest and highest.
        DVec3 lo(1e300);
        DVec3 hi(-1e300);
        for (std::uint32_t k = 0; k < kControls * kControls; ++k) {
            const Control& c = controls[std::size_t{i} * kControls * kControls + k];
            const DVec3 pos(c.position[0], c.position[1], c.position[2]);
            const DVec3 normal(c.up[0], c.up[1], c.up[2]);
            for (const double h :
                 {static_cast<double>(l.min_m) - static_cast<double>(t->skirt_m_[i]),
                  static_cast<double>(l.max_m)}) {
                const DVec3 q = pos + normal * h;
                lo = glm::min(lo, q);
                hi = glm::max(hi, q);
            }
        }
        const DVec3 mid = (lo + hi) * 0.5;
        tile.centre_ecef = t->origin_ecef_[i] + mid;
        tile.radius_m = glm::length(hi - mid) + 1.0;  // and float rounding
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
    const auto upload_buffer = [&](Buffer& dst, const void* data, std::size_t bytes,
                                   VkBufferUsageFlags usage, VkPipelineStageFlags2 stage,
                                   VkAccessFlags2 access) {
        dst = t->make_buffer(bytes, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
        // In pieces the staging buffer holds.
        const auto* src = static_cast<const std::uint8_t*>(data);
        for (std::size_t done = 0; done < bytes;) {
            const std::size_t part = std::min<std::size_t>(bytes - done, kStagingBytes - 16);
            up.reserve(part);
            VkBufferCopy copy{};
            copy.srcOffset = up.put(src + done, part);
            copy.dstOffset = done;
            copy.size = part;
            vkCmdCopyBuffer(up.cmd(), up.staging(), dst.buffer, 1, &copy);
            done += part;
        }
        VkMemoryBarrier2 mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        mb.dstStageMask = stage;
        mb.dstAccessMask = access;
        VkDependencyInfo d{};
        d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        d.memoryBarrierCount = 1;
        d.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(up.cmd(), &d);
    };
    upload_buffer(t->indices_, indices.data(), indices.size() * sizeof(std::uint32_t),
                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
                  VK_ACCESS_2_INDEX_READ_BIT);
    upload_buffer(t->controls_, controls.data(), controls.size() * sizeof(Control),
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT,
                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    up.flush();
    t->draws_ = t->make_buffer(VkDeviceSize{slots} * t->outputs_ * count * sizeof(TileDraw),
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    t->stack_.resize(count);
    t->selected_.resize(count);

    // Descriptors: one set, the heights and imagery as arrays indexed by
    // tile (descriptor indexing).
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
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count, VK_SHADER_STAGE_FRAGMENT_BIT,
                   nullptr};
    // The image arrays update after bind (paging, step 6, will fill
    // entries while frames using the set are in flight) and may be
    // partly bound.
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
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count * 2}};
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
    std::vector<VkDescriptorImageInfo> height_infos(count);
    std::vector<VkDescriptorImageInfo> imagery_infos(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        height_infos[i] = {VK_NULL_HANDLE, t->heights_[i].view,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkImageView iv =
            t->imagery_[i].view != VK_NULL_HANDLE ? t->imagery_[i].view : t->fallback_.view;
        imagery_infos[i] = {VK_NULL_HANDLE, iv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (std::uint32_t w = 0; w < writes.size(); ++w) {
        writes[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[w].dstSet = t->set_;
        writes[w].dstBinding = w;
        writes[w].descriptorCount = bindings[w].descriptorCount;
        writes[w].descriptorType = bindings[w].descriptorType;
    }
    writes[0].pBufferInfo = &controls_info;
    writes[1].pBufferInfo = &draws_info;
    writes[2].pImageInfo = &sampler_info;
    writes[3].pImageInfo = height_infos.data();
    writes[4].pImageInfo = imagery_infos.data();
    vkUpdateDescriptorSets(gpu.device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);

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

    report.tiles = count;
    report.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    return t;
}

TerrainGpu::~TerrainGpu() {
    // The caller has waited for the GPU.
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

std::uint32_t TerrainGpu::record(VkCommandBuffer cmd, std::uint32_t slot, std::uint32_t output,
                                 const SceneView& view, VkExtent2D extent) {
    const auto tiles = static_cast<std::uint32_t>(lod_.size());
    LodView lv;
    lv.eye_ecef = view.eye_ecef;
    lv.ecef_to_clip = view.ecef_to_clip;
    lv.pixels_per_tangent =
        static_cast<double>(extent.height) / std::max(static_cast<double>(view.tan_height), 1e-6);
    lv.pixel_error = pixel_error_;
    const auto drawn =
        static_cast<std::uint32_t>(select_tiles(lod_, roots_, lv, stack_, selected_));
    if (drawn == 0) {
        return 0;
    }
    const std::uint32_t base = (slot * outputs_ + output) * tiles;
    auto* records = static_cast<TileDraw*>(draws_.mapped) + base;
    for (std::uint32_t i = 0; i < drawn; ++i) {
        const std::uint32_t t = selected_[i];
        const DVec3 o = origin_ecef_[t] - view.eye_ecef;
        records[i] = {{static_cast<float>(o.x), static_cast<float>(o.y), static_cast<float>(o.z)},
                      t,
                      skirt_m_[t],
                      {}};
    }
    (void)vmaFlushAllocation(gpu_.allocator, draws_.memory, base * sizeof(TileDraw),
                             drawn * sizeof(TileDraw));

    TerrainPush push;
    push.ecef_to_clip = view.ecef_to_clip;
    const DVec3 sun = sun_direction(view.eye_ecef);
    push.sun = {static_cast<float>(sun.x), static_cast<float>(sun.y), static_cast<float>(sun.z)};
    push.samples = samples_;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set_, 0, nullptr);
    vkCmdBindIndexBuffer(cmd, indices_.buffer, 0, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push), &push);
    // The records' region: first instance is where this view's start.
    vkCmdDrawIndexed(cmd, index_count_, drawn, 0, 0, base);
    return drawn;
}

}  // namespace fovea
