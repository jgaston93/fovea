// The Vulkan backend: instance and device (vk-bootstrap, volk), memory (VMA),
// the two presenters behind Renderer, and the reference scene's pipelines.
//
// Frames: kFramesInFlight frames are recorded ahead, each with its own
// command buffer, fence and per-output acquire semaphores. Every swapchain
// image has its own "rendered" semaphore, since a present may still be
// using it when the next frame's fence has already signalled. Resources
// written every frame but not duplicated per frame (offscreen images and
// readback buffers, the HDR scene targets) are ordered after the previous
// frame's use with barriers.

#include <volk.h>

#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include "fovea/core/geodesy.hpp"
#include "fovea/render/renderer.hpp"
#include "overlay_ui.hpp"
#include "shaders/kCsAerial.hpp"
#include "shaders/kCsExposure.hpp"
#include "shaders/kCsIrradiance.hpp"
#include "shaders/kCsMultiScattering.hpp"
#include "shaders/kCsSkyView.hpp"
#include "shaders/kCsTransmittance.hpp"
#include "shaders/kFsModel.hpp"
#include "shaders/kFsModelShadow.hpp"
#include "shaders/kFsResolve.hpp"
#include "shaders/kFsScene.hpp"
#include "shaders/kFsStars.hpp"
#include "shaders/kFsTerrain.hpp"
#include "shaders/kFsTonemap.hpp"
#include "shaders/kVsFullscreen.hpp"
#include "shaders/kVsModel.hpp"
#include "shaders/kVsModelShadow.hpp"
#include "shaders/kVsStars.hpp"
#include "shaders/kVsTerrain.hpp"
#include "shaders/kVsTerrainShadow.hpp"
#include "shadows.hpp"
#include "sky.hpp"
#include "stars.hpp"
#include "terrain.hpp"

namespace fovea {

namespace {

// A band's kind as the shaders take it (weather.slang).
float band_kind(WeatherKind k) {
    switch (k) {
        case WeatherKind::rain:
            return 1.0F;
        case WeatherKind::snow:
            return 2.0F;
        default:
            return 0.0F;
    }
}

// A view's weather into its lighting block (step 6, D-034): the eye's
// height and up, and the fog, rain and snow, lowest first, each as its base,
// top, extinction (Koschmieder's 3.912 over its visibility) and kind (0
// fog, 1 rain, 2 snow: each under its own overcast). Cloud layers aren't
// drawn yet (#73): left out. No allocation.
void fill_weather(ViewLighting& l, std::span<const WeatherLayer> weather, double eye_height_m,
                  const DVec3& up) {
    l.eye_height_m = static_cast<float>(eye_height_m);
    l.up = {static_cast<float>(up.x), static_cast<float>(up.y), static_cast<float>(up.z)};
    std::array<const WeatherLayer*, kMaxWeatherLayers> bands{};
    std::size_t n = 0;
    for (const WeatherLayer& w : weather) {
        if (w.kind != WeatherKind::cloud && w.thickness_m > 0.0 && n < bands.size()) {
            bands[n++] = &w;
        }
    }
    std::sort(bands.begin(), bands.begin() + static_cast<std::ptrdiff_t>(n),
              [](const WeatherLayer* a, const WeatherLayer* b) { return a->base_m < b->base_m; });
    l.layer_count = static_cast<std::uint32_t>(n);
    for (std::size_t k = 0; k < n; ++k) {
        const WeatherLayer& w = *bands[k];
        l.layer[k] = {static_cast<float>(w.base_m), static_cast<float>(w.base_m + w.thickness_m),
                      static_cast<float>(3.912 / std::max(w.visibility_m, 1.0)), band_kind(w.kind)};
    }
}

constexpr std::uint32_t kFramesInFlight = 2;
// UNORM, so a clear to 200 shows 200 on screen (the identify check reads
// exact colours); the tone-map pass encodes sRGB itself for UNORM targets.
constexpr VkFormat kSwapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat kOffscreenFormat = VK_FORMAT_R8G8B8A8_UNORM;
// Linear, scene-referred light values (foundational decision 3).
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
// Reverse-Z: cleared to 0 (infinitely far), nearer is greater.
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

// volk keeps the Vulkan entry points in process-wide variables, so a second
// live Renderer would replace the first one's: one at a time.
std::atomic<bool> g_renderer_alive{false};

// The shaders' push-constant blocks (shaders/reference_scene.slang).
struct ScenePush {
    Mat4 clip_to_local{1.0F};
    float eye_height_m = 0.0F;
    float grid_spacing_m = 0.0F;
    float eye_offset_m[2] = {};  // NOLINT(modernize-avoid-c-arrays): the shader's float2
    std::uint32_t grid = 1;      // 0: terrain is loaded; no grid or horizon line
};
static_assert(sizeof(ScenePush) == 84, "must match ScenePush in reference_scene.slang");

struct TonemapPush {
    float exposure = 1.0F;
    std::uint32_t encode_srgb = 1;
    float unused[2] = {};  // NOLINT(modernize-avoid-c-arrays): matches the shader's float2
};
static_assert(sizeof(TonemapPush) == 16, "must match TonemapPush in reference_scene.slang");

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what);
}

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        fail(std::string(what) + " failed (VkResult " + std::to_string(static_cast<int>(r)) + ")");
    }
}

VkPresentModeKHR vk_present_mode(PresentMode mode) {
    switch (mode) {
        case PresentMode::fifo:
            return VK_PRESENT_MODE_FIFO_KHR;
        case PresentMode::mailbox:
            return VK_PRESENT_MODE_MAILBOX_KHR;
        case PresentMode::immediate:
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

// One sRGB-encoded channel (0..1) to linear: for *_SRGB targets, which encode
// on write, so the stored bytes still come out as the colour asked for.
float srgb_to_linear(float c) {
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

bool is_srgb(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB ||
           f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

VkClearColorValue clear_value(Rgb8 c, bool srgb_target) {
    const auto channel = [srgb_target](std::uint8_t v) {
        const float f = static_cast<float>(v) / 255.0F;
        return srgb_target ? srgb_to_linear(f) : f;
    };
    VkClearColorValue v{};
    v.float32[0] = channel(c.r);
    v.float32[1] = channel(c.g);
    v.float32[2] = channel(c.b);
    v.float32[3] = 1.0F;
    return v;
}

// The size a swapchain for this surface must have now, from the surface
// itself: SDL keeps reporting a minimized window's last size, while the
// surface reports 0x0. UINT32_MAX means the surface takes the swapchain's
// size; then the window's size from when the Renderer was made is used
// (clamped to the surface's limits), because this runs on the render thread
// and SDL's window functions belong to the main thread.
VkExtent2D surface_extent(VkPhysicalDevice gpu, VkSurfaceKHR surface, VkExtent2D window_size) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps) != VK_SUCCESS) {
        return {0, 0};
    }
    if (caps.currentExtent.width != UINT32_MAX) {
        return caps.currentExtent;
    }
    return {std::clamp(window_size.width, caps.minImageExtent.width, caps.maxImageExtent.width),
            std::clamp(window_size.height, caps.minImageExtent.height, caps.maxImageExtent.height)};
}

// One image layout transition, recorded with synchronization2.
void transition(VkCommandBuffer cmd, VkImage image, VkImageLayout from, VkImageLayout to,
                VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
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
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{};
    d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    d.imageMemoryBarrierCount = 1;
    d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &d);
}

void memory_barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                    VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 m{};
    m.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    m.srcStageMask = src_stage;
    m.srcAccessMask = src_access;
    m.dstStageMask = dst_stage;
    m.dstAccessMask = dst_access;
    VkDependencyInfo d{};
    d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    d.memoryBarrierCount = 1;
    d.pMemoryBarriers = &m;
    vkCmdPipelineBarrier2(cmd, &d);
}

// Draws the overlay's ImGui data over what a target already holds.
void draw_overlay(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent, ImDrawData* data) {
    // After the tone map's writes to the same target.
    memory_barrier(cmd, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                   VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo colour{};
    colour.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colour.imageView = target;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, extent};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &colour;
    vkCmdBeginRendering(cmd, &info);
    ImGui_ImplVulkan_RenderDrawData(data, cmd);
    vkCmdEndRendering(cmd);
}

// ImGui's memory through operator new, so allocation checks see it too.
void* imgui_alloc(std::size_t size, void* /*user*/) {
    return ::operator new(size);
}
void imgui_free(void* p, void* /*user*/) {
    ::operator delete(p);
}

// Builds this frame's overlay and draws it over the target.
// Returns false, drawing nothing, when ImGui wants a texture created or
// updated: its backend would upload it with vkQueueWaitIdle, blocking the
// render thread. enable_overlay uploads everything the overlay normally
// shows, so this means new content (a glyph never drawn before).
bool overlay_frame(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent,
                   const OverlayContent& content) {
    ImGui_ImplVulkan_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(extent.width), static_cast<float>(extent.height));
    io.DeltaTime = std::max(content.target_ms, 1.0F) / 1000.0F;
    ImGui::NewFrame();
    build_overlay_ui(content);
    ImGui::Render();
    const auto& textures = ImGui::GetPlatformIO().Textures;
    const bool pending = std::any_of(textures.begin(), textures.end(), [](const ImTextureData* t) {
        return t->Status != ImTextureStatus_OK;
    });
    if (pending) {
        return false;
    }
    // Null: the backend may not touch textures at all while drawing.
    ImDrawData* data = ImGui::GetDrawData();
    data->Textures = nullptr;
    draw_overlay(cmd, target, extent, data);
    return true;
}

// Begins dynamic rendering into one colour target, every pixel to be
// written, with a reverse-Z depth buffer cleared to 0 if one is given; the
// viewport and scissor cover the target.
void begin_pass(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent,
                VkImageView depth_view = VK_NULL_HANDLE,
                VkAttachmentLoadOp depth_load = VK_ATTACHMENT_LOAD_OP_CLEAR,
                VkAttachmentStoreOp depth_store = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                VkAttachmentLoadOp colour_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE) {
    VkRenderingAttachmentInfo colour{};
    colour.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colour.imageView = target;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = colour_load;  // usually every pixel is written
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth.imageView = depth_view;
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp = depth_load;
    depth.storeOp = depth_store;
    depth.clearValue.depthStencil = {0.0F, 0};
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, extent};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &colour;
    info.pDepthAttachment = depth_view != VK_NULL_HANDLE ? &depth : nullptr;
    vkCmdBeginRendering(cmd, &info);
    const VkViewport viewport{
        0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height),
        0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

// Draws the fullscreen triangle into one colour target (and a depth target
// it writes all of).
void draw_fullscreen(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent,
                     VkImageView depth_view = VK_NULL_HANDLE) {
    begin_pass(cmd, target, extent, depth_view, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
               VK_ATTACHMENT_STORE_OP_STORE);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
}

VKAPI_ATTR VkBool32 VKAPI_CALL on_validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* user) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        static_cast<std::atomic<std::uint64_t>*>(user)->fetch_add(1);
        (void)std::fprintf(
            stderr, "vulkan %s: %s\n",
            severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning",
            data != nullptr && data->pMessage != nullptr ? data->pMessage : "");
    }
    return VK_FALSE;
}

// shaders/resolve.slang's push constants.
struct ResolvePush {
    std::uint32_t samples = 0;
};
static_assert(sizeof(ResolvePush) == 4);

// The linear HDR image a view renders into before tone mapping.
struct HdrTarget {
    VkExtent2D extent{};
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;  // the tone-map pass samples it through this
    VkImage depth = VK_NULL_HANDLE;        // the scene's depth, for terrain
    VmaAllocation depth_memory = nullptr;
    VkImageView depth_view = VK_NULL_HANDLE;
    // Multisampled (milestone 5, D-027): the scene renders into these, and
    // the resolve (D-028) draws `image` and `depth` from them. Null when
    // single-sampled.
    VkImage ms = VK_NULL_HANDLE;
    VmaAllocation ms_memory = nullptr;
    VkImageView ms_view = VK_NULL_HANDLE;
    VkImage ms_depth = VK_NULL_HANDLE;
    VmaAllocation ms_depth_memory = nullptr;
    VkImageView ms_depth_view = VK_NULL_HANDLE;
    VkDescriptorSet resolve_set = VK_NULL_HANDLE;  // the resolve reads them through this
};

struct Swapchain {
    const Window* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D window_size{};      // read on the main thread when the Renderer was made
    std::uint64_t generation = 0;  // incremented by every rebuild
    vkb::Swapchain chain;
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkSemaphore> rendered;  // one per image
    bool needs_rebuild = false;
};

struct OffscreenImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation image_memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkBuffer readback = VK_NULL_HANDLE;
    VmaAllocation readback_memory = nullptr;
};

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    std::vector<VkSemaphore> acquired;  // one per swapchain
};

// One frame's work: clear each output to a colour, or render the scene.
struct DrawRequest {
    std::span<const Rgb8> colours;
    std::span<const SceneView> views;
    bool scene = false;
    const OverlayContent* overlay = nullptr;
};

}  // namespace

struct Renderer::Impl {
    std::atomic<std::uint64_t> messages{0};
    vkb::Instance instance;
    vkb::PhysicalDevice physical;
    vkb::Device device;
    VkQueue queue = VK_NULL_HANDLE;
    std::uint32_t queue_family = 0;
    VmaAllocator allocator = nullptr;
    GpuInfo info;
    std::vector<Swapchain> swapchains;
    std::vector<OffscreenImage> offscreen;
    std::array<Frame, kFramesInFlight> frames{};
    std::uint64_t frame_number = 0;
    bool owns_alive_flag = false;

    // The reference scene: one HDR target per output, two pipelines, and a
    // tone-map pipeline per output format (they rarely differ).
    std::vector<HdrTarget> hdr;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    VkPipelineLayout scene_layout = VK_NULL_HANDLE;
    VkPipelineLayout tonemap_layout = VK_NULL_HANDLE;
    VkShaderModule vs_fullscreen = VK_NULL_HANDLE;
    VkShaderModule fs_scene = VK_NULL_HANDLE;
    VkShaderModule fs_tonemap = VK_NULL_HANDLE;
    VkPipeline scene_pipeline = VK_NULL_HANDLE;
    std::vector<std::pair<VkFormat, VkPipeline>> tonemap_pipelines;
    // Multisampling (milestone 5, D-027, D-028).
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkShaderModule fs_resolve = VK_NULL_HANDLE;
    VkDescriptorSetLayout resolve_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout resolve_layout = VK_NULL_HANDLE;
    VkPipeline resolve_pipeline = VK_NULL_HANDLE;
    // Each view's lighting (milestone 6, D-029): a ViewLighting block per
    // frame slot and output, written as each view is recorded.
    SkyBodies sky = sky_bodies(EnvironmentState{}.julian_date);
    VkDescriptorSetLayout frame_layout = VK_NULL_HANDLE;
    VkDescriptorSet frame_set = VK_NULL_HANDLE;
    VkBuffer lighting = VK_NULL_HANDLE;
    VmaAllocation lighting_memory = nullptr;
    void* lighting_mapped = nullptr;
    VkDeviceSize lighting_stride = 0;
    // The sky's tables and each view's light (milestone 6 step 2), made
    // each frame before the views; the inputs sized once.
    std::unique_ptr<SkyGpu> sky_tables;
    std::unique_ptr<StarsGpu> stars;  // step 4
    // The weather's layers (step 6), as set_weather left them.
    FixedArray<WeatherLayer, kMaxWeatherLayers> weather;
    // The shadow maps (step 5): each view's cascades and their light this
    // frame (record_sky), and whether the terrain casts (the light low).
    std::unique_ptr<ShadowMaps> shadows;
    std::vector<ShadowCascades> view_cascades;
    std::vector<bool> terrain_casts;
    std::vector<bool> shadows_lit;  // a light casts this frame: else the maps are only cleared
    std::vector<SkyViewInput> sky_inputs;
    bool drew_scene = false;
    double frame_period_s = 1.0 / 60.0;  // RendererOptions::frame_rate_hz

    // What the terrain, models and sky need of the device (one place, so
    // the positional fields stay in step).
    GpuContext gpu_context() const {
        return {physical.physical_device,
                device.device,
                allocator,
                queue,
                queue_family,
                bc_textures,
                max_anisotropy,
                timeline,
                samples,
                frame_layout,
                frame_set,
                shadows ? shadows->maps() : VK_NULL_HANDLE,
                shadows ? shadows->sampler() : VK_NULL_HANDLE};
    }

    // Terrain, once loaded (milestone 3 step 5).
    std::unique_ptr<TerrainGpu> terrain;
    bool bc_textures = false;               // textureCompressionBC enabled
    float max_anisotropy = 0.0F;            // 0: samplerAnisotropy not enabled
    VkSemaphore timeline = VK_NULL_HANDLE;  // signalled with frame_number + 1 by each frame

    // Timing: two timestamps per output per frame in flight.
    VkQueryPool timestamps = VK_NULL_HANDLE;
    double timestamp_period_ns = 0.0;                         // 0: no timestamps on this queue
    std::uint64_t timestamp_mask = ~std::uint64_t{0};         // the queue's valid timestamp bits
    std::array<std::uint64_t, kFramesInFlight> slot_frame{};  // frame number in each slot
    std::vector<std::uint64_t> query_results;                 // sized once
    FrameTiming timing;

    // The debug overlay (Dear ImGui), on one output.
    bool overlay = false;
    std::uint32_t overlay_output = 0;
    VkFormat overlay_format = VK_FORMAT_UNDEFINED;  // ImGui's pipeline reads it by pointer
    ImGuiContext* imgui = nullptr;
    std::uint64_t overlay_skipped = 0;  // frames without the overlay: a texture was pending

    // Present pacing (VK_KHR_present_id / present_wait), when supported.
    bool present_wait = false;
    std::vector<std::uint64_t> present_ids;  // one per presented swapchain, sized once
    bool presented = false;                  // swapchain 0's last present was queued
    std::uint64_t presented_generation = 0;  // ...to this build of swapchain 0
    std::uint64_t last_present_id = 0;
    double present_wait_ms = 0.0;  // waited before this frame, for its timing

    // Per-frame scratch, sized once for the swapchain count: the render path
    // allocates nothing (architecture.md, "Render path rules").
    std::vector<std::uint32_t> image_index;
    std::vector<VkSemaphoreSubmitInfo> waits;
    std::vector<VkSemaphoreSubmitInfo> signals;
    std::vector<VkSwapchainKHR> present_chains;
    std::vector<std::uint32_t> present_indices;
    std::vector<VkSemaphore> present_waits;
    std::vector<std::size_t> present_which;
    std::vector<VkResult> present_results;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    void create_instance(const std::vector<std::string>& extensions, Validation validation);
    void create_device(bool present);
    void create_frames();
    void build_swapchain(Swapchain& s);
    void destroy_swapchain(Swapchain& s) const;
    void create_offscreen(OffscreenOutput o);
    void create_scene_resources(std::span<const VkExtent2D> render_extents);
    VkShaderModule make_module(std::span<const std::uint32_t> spirv) const;
    VkPipeline make_pipeline(VkPipelineLayout layout, VkShaderModule fragment, VkFormat format,
                             VkFormat depth = VK_FORMAT_UNDEFINED,
                             VkSampleCountFlagBits sample_count = VK_SAMPLE_COUNT_1_BIT,
                             bool write_depth = false) const;
    void choose_samples(std::uint32_t msaa);
    VkPipeline tonemap_pipeline(VkFormat format);
    VkImageView make_view(VkImage image, VkFormat format,
                          VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) const;
    void claim();
    void size_scratch();
    void create_timestamps();
    void read_gpu_times(std::size_t slot);
    std::uint32_t output_total() const {
        return static_cast<std::uint32_t>(swapchains.size() + offscreen.size());
    }
    Frame& begin_frame();
    std::uint32_t draw(const DrawRequest& request);
    void record_scene(VkCommandBuffer cmd, std::size_t slot, std::size_t i, const SceneView& view);
    // Each view's lighting into its slot's block, and the sky's passes.
    void record_sky(VkCommandBuffer cmd, std::size_t slot, std::span<const SceneView> views);
    std::uint32_t lighting_offset(std::size_t slot, std::size_t i) const {
        return static_cast<std::uint32_t>((slot * hdr.size() + i) * lighting_stride);
    }
};

void Renderer::Impl::create_instance(const std::vector<std::string>& extensions,
                                     Validation validation) {
    if (volkInitialize() != VK_SUCCESS) {
        fail("no Vulkan loader found");
    }
    vkb::InstanceBuilder builder(vkGetInstanceProcAddr);
    builder.set_app_name("Fovea").require_api_version(1, 3, 0);
    for (const std::string& e : extensions) {
        builder.enable_extension(e);
    }
    const auto system = vkb::SystemInfo::get_system_info(vkGetInstanceProcAddr);
    const bool have_layer = system && system->is_layer_available(kValidationLayer);
    if (validation == Validation::required && !have_layer) {
        fail(std::string("the Vulkan validation layer (") + kValidationLayer +
             ") is required but not installed");
    }
    info.validation = validation != Validation::off && have_layer;
    if (info.validation) {
        // Synchronization validation too: it checks barriers against the
        // commands themselves, so hazards that an in-order driver such as
        // lavapipe hides at run time are still reported.
        builder.enable_validation_layers(true)
            .add_validation_feature_enable(
                VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT)
            .set_debug_callback(&on_validation)
            .set_debug_callback_user_data_pointer(&messages)
            .set_debug_messenger_severity(VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT);
    }
    auto built = builder.build();
    if (!built) {
        fail("cannot create a Vulkan 1.3 instance: " + built.error().message());
    }
    instance = built.value();
    volkLoadInstanceOnly(instance.instance);
}

void Renderer::Impl::create_device(bool present) {
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    // discard in fragment shaders (models' alpha masks) is demote-to-helper,
    // which Vulkan 1.3 has everywhere but must be asked for.
    f13.shaderDemoteToHelperInvocation = VK_TRUE;
    // Descriptor indexing for bindless resources (architecture.md, "Render
    // path rules") and timeline semaphores for the paging thread.
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.timelineSemaphore = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;

    // Indirect draws of many primitives, each its own instance (models,
    // milestone 4 step 3: GPU-driven rendering, architecture.md).
    VkPhysicalDeviceFeatures f10{};
    f10.multiDrawIndirect = VK_TRUE;
    f10.drawIndirectFirstInstance = VK_TRUE;
    // Shadow casters beyond a map's box toward the light flattened onto it,
    // not clipped (milestone 6 step 5).
    f10.depthClamp = VK_TRUE;

    vkb::PhysicalDeviceSelector selector(instance);
    selector.set_minimum_version(1, 3)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
        .set_required_features(f10)
        .set_required_features_13(f13)
        .set_required_features_12(f12);
    if (present) {
        selector.set_surface(swapchains.front().surface)
            .add_required_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    } else {
        selector.require_present(false);
    }
    auto candidates = selector.select_devices();
    if (!candidates || candidates->empty()) {
        fail("no GPU with Vulkan 1.3 and the features Fovea needs" +
             (candidates ? std::string() : ": " + candidates.error().message()));
    }
    // Among the GPUs whose graphics queue can present to every window, the
    // first discrete one, else the first. The queue checked is the one used
    // below: vk-bootstrap's graphics queue, its first graphics family.
    const auto reaches_every_window = [&](const vkb::PhysicalDevice& d) {
        const auto families = d.get_queue_families();
        for (std::uint32_t q = 0; q < families.size(); ++q) {
            if ((families[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                continue;
            }
            bool all = true;
            for (const Swapchain& s : swapchains) {
                VkBool32 ok = VK_FALSE;
                all = all &&
                      vkGetPhysicalDeviceSurfaceSupportKHR(d.physical_device, q, s.surface, &ok) ==
                          VK_SUCCESS &&
                      ok == VK_TRUE;
            }
            return all;  // only the first graphics family counts
        }
        return false;
    };
    std::vector<vkb::PhysicalDevice> usable;
    for (const vkb::PhysicalDevice& d : *candidates) {
        if (reaches_every_window(d)) {
            usable.push_back(d);
        }
    }
    if (usable.empty()) {
        fail("no GPU can present to every window from one queue");
    }
    const auto discrete = std::find_if(usable.begin(), usable.end(), [](const auto& d) {
        return d.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    });
    physical = discrete != usable.end() ? *discrete : usable.front();

    if (present) {
        // Optional: present ids and waits, for evenly paced frames.
        VkPhysicalDevicePresentIdFeaturesKHR id{};
        id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
        id.presentId = VK_TRUE;
        VkPhysicalDevicePresentWaitFeaturesKHR wait{};
        wait.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
        wait.presentWait = VK_TRUE;
        // enable_extension_features_if_present asks the driver;
        // are_extension_features_present only knows the features asked for
        // during selection, so it says no here.
        if (physical.is_extension_present(VK_KHR_PRESENT_ID_EXTENSION_NAME) &&
            physical.is_extension_present(VK_KHR_PRESENT_WAIT_EXTENSION_NAME)) {
            (void)physical.enable_extension_if_present(VK_KHR_PRESENT_ID_EXTENSION_NAME);
            (void)physical.enable_extension_if_present(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
            const bool have_id = physical.enable_extension_features_if_present(id);
            const bool have_wait = physical.enable_extension_features_if_present(wait);
            present_wait = have_id && have_wait;
        }
    }

    // Optional: BC textures (terrain imagery, D-017) and anisotropic
    // filtering, where the GPU has them.
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(physical.physical_device, &have);
    VkPhysicalDeviceFeatures want{};
    want.textureCompressionBC = have.textureCompressionBC;
    want.samplerAnisotropy = have.samplerAnisotropy;
    if (physical.enable_features_if_present(want)) {
        bc_textures = want.textureCompressionBC == VK_TRUE;
        max_anisotropy = want.samplerAnisotropy == VK_TRUE
                             ? physical.properties.limits.maxSamplerAnisotropy
                             : 0.0F;
    }

    auto built = vkb::DeviceBuilder(physical).build();
    if (!built) {
        fail("cannot create the Vulkan device: " + built.error().message());
    }
    device = built.value();
    volkLoadDevice(device.device);
    auto q = device.get_queue(vkb::QueueType::graphics);
    auto qi = device.get_queue_index(vkb::QueueType::graphics);
    if (!q || !qi) {
        fail("the GPU has no graphics queue");
    }
    queue = q.value();
    queue_family = qi.value();

    // One queue draws and presents everything, so it must reach every window.
    for (const Swapchain& s : swapchains) {
        VkBool32 supported = VK_FALSE;
        check(vkGetPhysicalDeviceSurfaceSupportKHR(physical.physical_device, queue_family,
                                                   s.surface, &supported),
              "vkGetPhysicalDeviceSurfaceSupportKHR");
        if (supported != VK_TRUE) {
            fail(physical.name + " cannot present to every window");
        }
    }

    VkPhysicalDeviceDriverProperties driver{};
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    VkPhysicalDeviceProperties2 props{};
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    props.pNext = &driver;
    vkGetPhysicalDeviceProperties2(physical.physical_device, &props);
    info.name = props.properties.deviceName;
    info.driver = driver.driverName;
    info.discrete = props.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    info.api_major = VK_API_VERSION_MAJOR(props.properties.apiVersion);
    info.api_minor = VK_API_VERSION_MINOR(props.properties.apiVersion);

    VmaVulkanFunctions functions{};
    VmaAllocatorCreateInfo ci{};
    ci.vulkanApiVersion = VK_API_VERSION_1_3;
    ci.instance = instance.instance;
    ci.physicalDevice = physical.physical_device;
    ci.device = device.device;
    check(vmaImportVulkanFunctionsFromVolk(&ci, &functions), "vmaImportVulkanFunctionsFromVolk");
    ci.pVulkanFunctions = &functions;
    check(vmaCreateAllocator(&ci, &allocator), "vmaCreateAllocator");
}

void Renderer::Impl::create_frames() {
    for (Frame& f : frames) {
        VkCommandPoolCreateInfo pool{};
        pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = queue_family;
        check(vkCreateCommandPool(device.device, &pool, nullptr, &f.pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = f.pool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device.device, &alloc, &f.cmd), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fence{};
        fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        check(vkCreateFence(device.device, &fence, nullptr, &f.done), "vkCreateFence");
        VkSemaphoreCreateInfo sem{};
        sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        f.acquired.resize(swapchains.size());
        for (VkSemaphore& s : f.acquired) {
            check(vkCreateSemaphore(device.device, &sem, nullptr, &s), "vkCreateSemaphore");
        }
    }
    VkSemaphoreTypeCreateInfo type{};
    type.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo tci{};
    tci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    tci.pNext = &type;
    check(vkCreateSemaphore(device.device, &tci, nullptr, &timeline),
          "vkCreateSemaphore (timeline)");
}

VkImageView Renderer::Impl::make_view(VkImage image, VkFormat format,
                                      VkImageAspectFlags aspect) const {
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    check(vkCreateImageView(device.device, &vci, nullptr, &view), "vkCreateImageView");
    return view;
}

void Renderer::Impl::build_swapchain(Swapchain& s) {
    const VkExtent2D size = surface_extent(physical.physical_device, s.surface, s.window_size);
    if (size.width == 0 || size.height == 0) {
        s.needs_rebuild = true;  // minimized or not sized yet: build when drawable
        return;
    }
    vkb::SwapchainBuilder builder(device, s.surface);
    builder.set_desired_format({kSwapchainFormat, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR})
        .set_desired_present_mode(s.present_mode)
        .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR)  // always supported
        .set_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        .set_desired_extent(size.width, size.height);
    if (s.chain.swapchain != VK_NULL_HANDLE) {
        builder.set_old_swapchain(s.chain);
    }
    auto built = builder.build();
    if (!built) {
        fail("cannot create a swapchain: " + built.error().message());
    }
    destroy_swapchain(s);  // the old one, now retired
    s.chain = built.value();
    ++s.generation;
    auto images = s.chain.get_images();
    if (!images) {
        fail("cannot get swapchain images: " + images.error().message());
    }
    s.images = images.value();
    VkSemaphoreCreateInfo sem{};
    sem.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    s.rendered.resize(s.images.size());
    s.views.resize(s.images.size());
    for (std::size_t k = 0; k < s.images.size(); ++k) {
        check(vkCreateSemaphore(device.device, &sem, nullptr, &s.rendered[k]), "vkCreateSemaphore");
        s.views[k] = make_view(s.images[k], s.chain.image_format);
    }
    // The tone-map pass's pipeline for this format, made here (not while
    // drawing) when it is new.
    if (scene_layout != VK_NULL_HANDLE) {
        (void)tonemap_pipeline(s.chain.image_format);
    }
    s.needs_rebuild = false;
}

void Renderer::Impl::destroy_swapchain(Swapchain& s) const {
    for (VkImageView v : s.views) {
        vkDestroyImageView(device.device, v, nullptr);
    }
    s.views.clear();
    for (VkSemaphore r : s.rendered) {
        vkDestroySemaphore(device.device, r, nullptr);
    }
    s.rendered.clear();
    s.images.clear();
    if (s.chain.swapchain != VK_NULL_HANDLE) {
        vkb::destroy_swapchain(s.chain);
        s.chain = {};
    }
}

void Renderer::Impl::create_offscreen(OffscreenOutput o) {
    OffscreenImage img;
    img.width = o.width;
    img.height = o.height;
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = kOffscreenFormat;
    ici.extent = {o.width, o.height, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    check(vmaCreateImage(allocator, &ici, &aci, &img.image, &img.image_memory, nullptr),
          "vmaCreateImage");
    img.view = make_view(img.image, kOffscreenFormat);
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = static_cast<VkDeviceSize>(o.width) * o.height * 4;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo rci{};
    rci.usage = VMA_MEMORY_USAGE_AUTO;
    rci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
    check(vmaCreateBuffer(allocator, &bci, &rci, &img.readback, &img.readback_memory, nullptr),
          "vmaCreateBuffer");
    offscreen.push_back(img);
}

VkShaderModule Renderer::Impl::make_module(std::span<const std::uint32_t> spirv) const {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv.size_bytes();
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    check(vkCreateShaderModule(device.device, &ci, nullptr, &m), "vkCreateShaderModule");
    return m;
}

VkPipeline Renderer::Impl::make_pipeline(VkPipelineLayout layout, VkShaderModule fragment,
                                         VkFormat format, VkFormat depth,
                                         VkSampleCountFlagBits sample_count,
                                         bool write_depth) const {
    std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_fullscreen;
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
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0F;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = sample_count;
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
    rendering.pColorAttachmentFormats = &format;
    rendering.depthAttachmentFormat = depth;
    // With a depth attachment, a fullscreen pass neither tests nor writes it
    // (the sky leaves the clear), unless it writes every pixel's (the
    // resolve: the samples' nearest).
    VkPipelineDepthStencilStateCreateInfo depth_state{};
    depth_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    if (write_depth) {
        depth_state.depthTestEnable = VK_TRUE;
        depth_state.depthWriteEnable = VK_TRUE;
        depth_state.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    }

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
    ci.pDepthStencilState = depth != VK_FORMAT_UNDEFINED ? &depth_state : nullptr;
    ci.pColorBlendState = &blend;
    ci.pDynamicState = &dynamic;
    ci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    check(vkCreateGraphicsPipelines(device.device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline),
          "vkCreateGraphicsPipelines");
    return pipeline;
}

VkPipeline Renderer::Impl::tonemap_pipeline(VkFormat format) {
    for (const auto& [f, p] : tonemap_pipelines) {
        if (f == format) {
            return p;
        }
    }
    VkPipeline p = make_pipeline(tonemap_layout, fs_tonemap, format);  // a handle
    tonemap_pipelines.emplace_back(format, p);
    return p;
}

void Renderer::Impl::choose_samples(std::uint32_t msaa) {
    // Supported for both the HDR colour and the depth format, drawn into and
    // read by the resolve, as the device and the formats themselves allow.
    const VkPhysicalDeviceLimits& limits = physical.properties.limits;
    VkSampleCountFlags ok =
        limits.framebufferColorSampleCounts & limits.framebufferDepthSampleCounts;
    const auto format_counts = [&](VkFormat f, VkImageUsageFlags usage) {
        VkImageFormatProperties props{};
        return vkGetPhysicalDeviceImageFormatProperties(physical.physical_device, f,
                                                        VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                        usage, 0, &props) == VK_SUCCESS
                   ? props.sampleCounts
                   : VkSampleCountFlags{0};
    };
    ok &= limits.sampledImageColorSampleCounts & limits.sampledImageDepthSampleCounts;
    ok &=
        format_counts(kHdrFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    ok &= format_counts(kDepthFormat,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    const bool known = msaa == 1 || msaa == 2 || msaa == 4 || msaa == 8;
    if (!known || (ok & msaa) == 0) {
        std::string can;
        for (const std::uint32_t c : {1U, 2U, 4U, 8U}) {
            if ((ok & c) != 0) {
                can += (can.empty() ? "" : ", ") + std::to_string(c);
            }
        }
        fail("msaa " + std::to_string(msaa) + ": this GPU can do " + can);
    }
    samples = static_cast<VkSampleCountFlagBits>(msaa);
}

void Renderer::Impl::create_scene_resources(std::span<const VkExtent2D> render_extents) {
    vs_fullscreen = make_module(kVsFullscreen);
    fs_scene = make_module(kFsScene);
    fs_tonemap = make_module(kFsTonemap);

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    check(vkCreateSampler(device.device, &sci, nullptr, &sampler), "vkCreateSampler");

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{};
    lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    lci.bindingCount = 1;
    lci.pBindings = &binding;
    check(vkCreateDescriptorSetLayout(device.device, &lci, nullptr, &set_layout),
          "vkCreateDescriptorSetLayout");

    // The resolve's: the multisampled colour and depth.
    std::array<VkDescriptorSetLayoutBinding, 2> ms_bindings{};
    for (std::uint32_t b = 0; b < ms_bindings.size(); ++b) {
        ms_bindings[b].binding = b;
        ms_bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        ms_bindings[b].descriptorCount = 1;
        ms_bindings[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ms_lci{};
    ms_lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    ms_lci.bindingCount = static_cast<std::uint32_t>(ms_bindings.size());
    ms_lci.pBindings = ms_bindings.data();
    check(vkCreateDescriptorSetLayout(device.device, &ms_lci, nullptr, &resolve_set_layout),
          "vkCreateDescriptorSetLayout (resolve)");

    // The lighting's: one dynamic uniform buffer, offset to the view's block.
    // The sky's (milestone 6 step 2): each view's light, the views'
    // sky-view tables and the atmosphere (shaders/include/frame.slang).
    const std::array<VkDescriptorType, 8> frame_types = {
        VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          VK_DESCRIPTOR_TYPE_SAMPLER};
    std::array<VkDescriptorSetLayoutBinding, 8> frame_bindings{};
    for (std::uint32_t b = 0; b < frame_bindings.size(); ++b) {
        frame_bindings[b].binding = b;
        frame_bindings[b].descriptorType = frame_types[b];
        frame_bindings[b].descriptorCount = 1;
        frame_bindings[b].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo frame_lci{};
    frame_lci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    frame_lci.bindingCount = static_cast<std::uint32_t>(frame_bindings.size());
    frame_lci.pBindings = frame_bindings.data();
    check(vkCreateDescriptorSetLayout(device.device, &frame_lci, nullptr, &frame_layout),
          "vkCreateDescriptorSetLayout (frame)");

    const auto count = static_cast<std::uint32_t>(render_extents.size());
    const std::array<VkDescriptorPoolSize, 6> pool_sizes = {
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count + 3},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, count * 2 + 1},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = count * 2 + 1;
    pci.poolSizeCount = static_cast<std::uint32_t>(pool_sizes.size());
    pci.pPoolSizes = pool_sizes.data();
    check(vkCreateDescriptorPool(device.device, &pci, nullptr, &descriptor_pool),
          "vkCreateDescriptorPool");

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical.physical_device, &props);
    const VkDeviceSize align =
        std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 1);
    lighting_stride = (sizeof(ViewLighting) + align - 1) / align * align;
    VkBufferCreateInfo lbci{};
    lbci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    lbci.size = lighting_stride * kFramesInFlight * count;
    lbci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo laci{};
    laci.usage = VMA_MEMORY_USAGE_AUTO;
    laci.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo linfo{};
    check(vmaCreateBuffer(allocator, &lbci, &laci, &lighting, &lighting_memory, &linfo),
          "vmaCreateBuffer (lighting)");
    lighting_mapped = linfo.pMappedData;
    VkDescriptorSetAllocateInfo fai{};
    fai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    fai.descriptorPool = descriptor_pool;
    fai.descriptorSetCount = 1;
    fai.pSetLayouts = &frame_layout;
    check(vkAllocateDescriptorSets(device.device, &fai, &frame_set),
          "vkAllocateDescriptorSets (frame)");

    const GpuContext gpu = gpu_context();
    const std::array<VkShaderModule, 6> sky_modules = {
        make_module(kCsTransmittance), make_module(kCsMultiScattering), make_module(kCsSkyView),
        make_module(kCsIrradiance),    make_module(kCsExposure),        make_module(kCsAerial)};
    try {
        sky_tables = std::make_unique<SkyGpu>(gpu, count, Atmosphere{}, sky_modules);
    } catch (...) {
        for (VkShaderModule m : sky_modules) {
            vkDestroyShaderModule(device.device, m, nullptr);
        }
        throw;
    }
    for (VkShaderModule m : sky_modules) {
        vkDestroyShaderModule(device.device, m, nullptr);
    }
    sky_tables->set_visibility(EnvironmentState{}.visibility_m);
    VkShaderModule vs_stars = make_module(kVsStars);
    VkShaderModule fs_stars = make_module(kFsStars);
    try {
        stars = std::make_unique<StarsGpu>(gpu, kHdrFormat, kDepthFormat, vs_stars, fs_stars);
    } catch (...) {
        vkDestroyShaderModule(device.device, vs_stars, nullptr);
        vkDestroyShaderModule(device.device, fs_stars, nullptr);
        throw;
    }
    vkDestroyShaderModule(device.device, vs_stars, nullptr);
    vkDestroyShaderModule(device.device, fs_stars, nullptr);
    sky_inputs.resize(count);

    const VkDescriptorBufferInfo lighting_info{lighting, 0, sizeof(ViewLighting)};
    const VkDescriptorBufferInfo view_sky_info{sky_tables->view_sky(), 0,
                                               sky_tables->view_sky_size()};
    const VkDescriptorImageInfo sky_view_info{sky_tables->sampler(), sky_tables->sky_view_tables(),
                                              VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorBufferInfo atmosphere_info{sky_tables->atmosphere_buffer(), 0,
                                                 SkyGpu::atmosphere_size()};
    const VkDescriptorImageInfo aerial_info{sky_tables->sampler(), sky_tables->aerial_volumes(),
                                            VK_IMAGE_LAYOUT_GENERAL};
    const VkDescriptorImageInfo transmittance_info{
        sky_tables->sampler(), sky_tables->transmittance_table(), VK_IMAGE_LAYOUT_GENERAL};
    // The shadow maps (bindings 6 and 7) come with terrain (load_terrain):
    // nothing else casts or reads them.
    view_cascades.resize(count);
    terrain_casts.assign(count, false);
    shadows_lit.assign(count, false);
    std::array<VkWriteDescriptorSet, 6> fws{};
    for (std::uint32_t b = 0; b < fws.size(); ++b) {
        fws[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        fws[b].dstSet = frame_set;
        fws[b].dstBinding = b;
        fws[b].descriptorCount = 1;
        fws[b].descriptorType = frame_types[b];
    }
    fws[0].pBufferInfo = &lighting_info;
    fws[1].pBufferInfo = &view_sky_info;
    fws[2].pImageInfo = &sky_view_info;
    fws[3].pBufferInfo = &atmosphere_info;
    fws[4].pImageInfo = &aerial_info;
    fws[5].pImageInfo = &transmittance_info;
    vkUpdateDescriptorSets(device.device, static_cast<std::uint32_t>(fws.size()), fws.data(), 0,
                           nullptr);

    VkPushConstantRange scene_push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScenePush)};
    // The frame set is the scene pass's set 1; set 0, the tone map's, it
    // leaves unbound.
    const std::array<VkDescriptorSetLayout, 2> scene_sets = {set_layout, frame_layout};
    VkPipelineLayoutCreateInfo scene_lci{};
    scene_lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    scene_lci.setLayoutCount = static_cast<std::uint32_t>(scene_sets.size());
    scene_lci.pSetLayouts = scene_sets.data();
    scene_lci.pushConstantRangeCount = 1;
    scene_lci.pPushConstantRanges = &scene_push;
    check(vkCreatePipelineLayout(device.device, &scene_lci, nullptr, &scene_layout),
          "vkCreatePipelineLayout");
    VkPushConstantRange tonemap_push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TonemapPush)};
    VkPipelineLayoutCreateInfo tonemap_lci{};
    tonemap_lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    tonemap_lci.setLayoutCount = 1;
    tonemap_lci.pSetLayouts = &set_layout;
    tonemap_lci.pushConstantRangeCount = 1;
    tonemap_lci.pPushConstantRanges = &tonemap_push;
    check(vkCreatePipelineLayout(device.device, &tonemap_lci, nullptr, &tonemap_layout),
          "vkCreatePipelineLayout");

    scene_pipeline = make_pipeline(scene_layout, fs_scene, kHdrFormat, kDepthFormat, samples);
    if (samples != VK_SAMPLE_COUNT_1_BIT) {
        fs_resolve = make_module(kFsResolve);
        VkPushConstantRange resolve_push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ResolvePush)};
        VkPipelineLayoutCreateInfo resolve_lci{};
        resolve_lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        resolve_lci.setLayoutCount = 1;
        resolve_lci.pSetLayouts = &resolve_set_layout;
        resolve_lci.pushConstantRangeCount = 1;
        resolve_lci.pPushConstantRanges = &resolve_push;
        check(vkCreatePipelineLayout(device.device, &resolve_lci, nullptr, &resolve_layout),
              "vkCreatePipelineLayout (resolve)");
        resolve_pipeline = make_pipeline(resolve_layout, fs_resolve, kHdrFormat, kDepthFormat,
                                         VK_SAMPLE_COUNT_1_BIT, true);
    }
    (void)tonemap_pipeline(kOffscreenFormat);
    for (const Swapchain& s : swapchains) {
        if (s.chain.swapchain != VK_NULL_HANDLE) {
            (void)tonemap_pipeline(s.chain.image_format);
        }
    }

    hdr.reserve(render_extents.size());
    for (const VkExtent2D extent : render_extents) {
        // In hdr from the start: the destructor frees what a failure part
        // way leaves (null handles are ignored).
        HdrTarget& t = hdr.emplace_back();
        t.extent = extent;
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = kHdrFormat;
        ici.extent = {extent.width, extent.height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        check(vmaCreateImage(allocator, &ici, &aci, &t.image, &t.memory, nullptr),
              "vmaCreateImage");
        t.view = make_view(t.image, kHdrFormat);
        ici.format = kDepthFormat;
        ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        check(vmaCreateImage(allocator, &ici, &aci, &t.depth, &t.depth_memory, nullptr),
              "vmaCreateImage (depth)");
        t.depth_view = make_view(t.depth, kDepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (samples != VK_SAMPLE_COUNT_1_BIT) {
            ici.samples = samples;
            ici.format = kHdrFormat;
            ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            check(vmaCreateImage(allocator, &ici, &aci, &t.ms, &t.ms_memory, nullptr),
                  "vmaCreateImage (multisampled)");
            t.ms_view = make_view(t.ms, kHdrFormat);
            ici.format = kDepthFormat;
            ici.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
            check(vmaCreateImage(allocator, &ici, &aci, &t.ms_depth, &t.ms_depth_memory, nullptr),
                  "vmaCreateImage (multisampled depth)");
            t.ms_depth_view = make_view(t.ms_depth, kDepthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
            VkDescriptorSetAllocateInfo rai{};
            rai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            rai.descriptorPool = descriptor_pool;
            rai.descriptorSetCount = 1;
            rai.pSetLayouts = &resolve_set_layout;
            check(vkAllocateDescriptorSets(device.device, &rai, &t.resolve_set),
                  "vkAllocateDescriptorSets (resolve)");
            const std::array<VkDescriptorImageInfo, 2> ms_info = {
                VkDescriptorImageInfo{VK_NULL_HANDLE, t.ms_view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                VkDescriptorImageInfo{VK_NULL_HANDLE, t.ms_depth_view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = t.resolve_set;
            w.descriptorCount = static_cast<std::uint32_t>(ms_info.size());
            w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w.pImageInfo = ms_info.data();
            vkUpdateDescriptorSets(device.device, 1, &w, 0, nullptr);
        }
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descriptor_pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout;
        check(vkAllocateDescriptorSets(device.device, &ai, &t.set), "vkAllocateDescriptorSets");
        VkDescriptorImageInfo image_info{sampler, t.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = t.set;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &image_info;
        vkUpdateDescriptorSets(device.device, 1, &write, 0, nullptr);
    }
}

void Renderer::Impl::claim() {
    if (g_renderer_alive.exchange(true)) {
        fail("only one Renderer may exist at a time");
    }
    owns_alive_flag = true;
}

void Renderer::Impl::size_scratch() {
    const std::size_t n = swapchains.size();
    image_index.resize(n);
    waits.resize(n);
    signals.resize(n + 1);  // and the timeline
    present_chains.resize(n);
    present_indices.resize(n);
    present_waits.resize(n);
    present_which.resize(n);
    present_results.resize(n);
    present_ids.resize(n);
}

void Renderer::Impl::create_timestamps() {
    const auto families = physical.get_queue_families();
    if (queue_family >= families.size() || families[queue_family].timestampValidBits == 0) {
        return;  // no GPU timings on this queue
    }
    timestamp_period_ns = static_cast<double>(physical.properties.limits.timestampPeriod);
    const std::uint32_t bits = families[queue_family].timestampValidBits;
    timestamp_mask = bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
    const std::uint32_t count = kFramesInFlight * output_total() * 2;
    VkQueryPoolCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    ci.queryCount = count;
    check(vkCreateQueryPool(device.device, &ci, nullptr, &timestamps), "vkCreateQueryPool");
    query_results.resize(static_cast<std::size_t>(output_total()) * std::size_t{2});
}

// The GPU times of the frame last drawn in this slot, now that its fence has
// signalled.
void Renderer::Impl::read_gpu_times(std::size_t slot) {
    if (timestamps == VK_NULL_HANDLE || frame_number < kFramesInFlight) {
        return;  // nothing recorded in this slot yet
    }
    const std::uint32_t n = output_total() * 2;
    const VkResult r =
        vkGetQueryPoolResults(device.device, timestamps, static_cast<std::uint32_t>(slot) * n, n,
                              query_results.size() * sizeof(std::uint64_t), query_results.data(),
                              sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT);
    if (r != VK_SUCCESS) {
        return;
    }
    timing.gpu_frame = slot_frame[slot];
    timing.gpu_outputs = std::min<std::uint32_t>(output_total(), kMaxTimedOutputs);
    for (std::uint32_t i = 0; i < timing.gpu_outputs; ++i) {
        const std::size_t k = std::size_t{2} * i;
        // Modulo the valid bits: the counter may wrap between the two.
        const std::uint64_t ticks = (query_results[k + 1] - query_results[k]) & timestamp_mask;
        timing.gpu_ms[i] =
            static_cast<float>(static_cast<double>(ticks) * timestamp_period_ns * 1e-6);
    }
}

Frame& Renderer::Impl::begin_frame() {
    const std::size_t slot = frame_number % kFramesInFlight;
    Frame& f = frames[slot];
    check(vkWaitForFences(device.device, 1, &f.done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    read_gpu_times(slot);
    return f;
}

// The scene for output i into its HDR target, ready for the tone-map pass:
// the reference scene (sky and grid), then any terrain over it.
void Renderer::Impl::record_sky(VkCommandBuffer cmd, std::size_t slot,
                                std::span<const SceneView> views) {
    if (shadows) {
        // Every view's maps readable before any view draws (its own are then
        // redrawn before its scene, when a light casts).
        shadows->ready(cmd);
    }
    for (std::size_t i = 0; i < hdr.size(); ++i) {
        const SceneView view = i < views.size() ? views[i] : SceneView{};
        // The sun from the Earth's centre (its parallax is 9 arc-seconds),
        // the moon from the eye.
        const Geodetic eye = ecef_to_geodetic(view.eye_ecef);
        const DMat3 level = ned_to_ecef(eye.lat_deg, eye.lon_deg);
        const DVec3 up = level * DVec3(0.0, 0.0, -1.0);
        const DVec3 moon = glm::normalize(sky.moon_m - view.eye_ecef);
        const DVec3 sun_local = glm::transpose(level) * sky.sun;
        ViewLighting l;
        l.sun = {static_cast<float>(sky.sun.x), static_cast<float>(sky.sun.y),
                 static_cast<float>(sky.sun.z)};
        l.moon_lit = static_cast<float>(sky.moon_lit);
        l.moon = {static_cast<float>(moon.x), static_cast<float>(moon.y),
                  static_cast<float>(moon.z)};
        const double sun_up = disc_above_horizon(sky.sun, up, kSunRadiusDeg);
        l.sun_up = static_cast<float>(sun_up);
        l.sun_local = {static_cast<float>(sun_local.x), static_cast<float>(sun_local.y),
                       static_cast<float>(sun_local.z)};
        l.view = static_cast<std::uint32_t>(i);
        const double height_km = std::max(eye.alt_m, 0.0) / 1000.0;
        l.eye_radius_km = static_cast<float>(sky_tables->atmosphere().bottom_radius_km + height_km);
        l.inv_width = 1.0F / static_cast<float>(std::max(hdr[i].extent.width, 1U));
        l.inv_height = 1.0F / static_cast<float>(std::max(hdr[i].extent.height, 1U));
        l.views = sky_tables->views();  // the volumes' stacking, as SkyGpu made them
        const DVec3 moon_local = glm::transpose(level) * moon;
        const double moon_up = disc_above_horizon(moon, up, kMoonRadiusDeg);
        l.moon_local = {static_cast<float>(moon_local.x), static_cast<float>(moon_local.y),
                        static_cast<float>(moon_local.z)};
        l.moon_up = static_cast<float>(moon_up);
        // The shadows' light (step 5): the sun while any of it is up, else
        // the moon, else none; their cascades for this view.
        // A moon all but new lights nothing worth a shadow.
        const bool by_sun = sun_up > 0.0;
        const bool by_moon = !by_sun && moon_up > 0.0 && sky.moon_lit > 0.1;
        shadows_lit[i] = by_sun || by_moon;
        if (by_sun || by_moon) {
            const DVec3 towards = by_sun ? sky.sun : moon;
            view_cascades[i] = fit_shadow_cascades(view.eye_ecef, view.ecef_to_clip, towards);
            // The terrain casts only with the light under 30 degrees: higher,
            // its imagery holds its shadows (D-032).
            terrain_casts[i] = glm::dot(towards, up) < std::sin(glm::radians(30.0));
            l.shadow_light = by_sun ? 1U : 2U;
            for (std::size_t c = 0; c < kShadowCascades; ++c) {
                l.cascade[c] = Mat4(view_cascades[i].to_map[c]);
                l.cascade_texel[c] = static_cast<float>(view_cascades[i].texel_m[c]);
            }
        } else {
            terrain_casts[i] = false;
            l.shadow_light = 0;
        }
        fill_weather(l, weather, eye.alt_m, up);
        const std::uint32_t at = lighting_offset(slot, i);
        std::memcpy(static_cast<std::uint8_t*>(lighting_mapped) + at, &l, sizeof(l));
        (void)vmaFlushAllocation(allocator, lighting_memory, at, sizeof(l));
        sky_inputs[i] = {height_km,          -sun_local.z,    sun_up,
                         view.clip_to_local, Vec3(sun_local), sky.moon_illuminance * moon_up,
                         Vec3(moon_local)};
    }
    // Exposure moves toward its target with a time constant of a second
    // (D-031), a frame period a frame: by the frame clock, not the wall's,
    // so a replay drawn faster than real time sees what the live run did.
    // The first frame takes it at once.
    const float adapt =
        drew_scene ? static_cast<float>(1.0 - std::exp(-frame_period_s / 1.0)) : 1.0F;
    drew_scene = true;
    sky_tables->record(cmd, sky_inputs, adapt);
}

void Renderer::Impl::record_scene(VkCommandBuffer cmd, std::size_t slot, std::size_t i,
                                  const SceneView& view) {
    const HdrTarget& t = hdr[i];
    const bool multisampled = t.ms != VK_NULL_HANDLE;
    if (terrain) {
        // The view's tiles, then, when a light casts, its shadow maps from
        // it (step 5): the casters' depths, before the scene that samples
        // them. Without one, the maps aren't read (shadow_light 0).
        const auto s = static_cast<std::uint32_t>(slot);
        const auto o = static_cast<std::uint32_t>(i);
        (void)terrain->prepare(s, o, view, t.extent);
        if (shadows_lit[i]) {
            for (std::uint32_t c = 0; c < kShadowCascades; ++c) {
                shadows->begin(cmd, o, c);
                terrain->draw_shadow(cmd, s, o, c, view, Mat4(view_cascades[i].to_map[c]),
                                     terrain_casts[i]);
                vkCmdEndRendering(cmd);
            }
            shadows->done(cmd, o);
        }
    }
    // The scene's colour target (the HDR target itself, or its multisampled
    // twin), after the previous frame's tone-map or resolve pass read it
    // (write after read: an execution dependency is enough).
    transition(cmd, multisampled ? t.ms : t.image, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene_pipeline);
    ScenePush push;
    push.clip_to_local = view.clip_to_local;
    push.eye_height_m = view.eye_height_m;
    push.grid_spacing_m = view.grid_spacing_m;
    push.eye_offset_m[0] = view.eye_offset_m.x;
    push.eye_offset_m[1] = view.eye_offset_m.y;
    push.grid = terrain ? 0U : 1U;
    vkCmdPushConstants(cmd, scene_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    const std::uint32_t offset = lighting_offset(slot, i);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene_layout, 1, 1, &frame_set, 1,
                            &offset);
    // The depth buffer the scene draws into, cleared each frame: after the
    // previous frame's tests and writes to it (and, multisampled, the
    // resolve's reads of it).
    std::array<VkImageMemoryBarrier2, 2> dbs{};
    for (VkImageMemoryBarrier2& db : dbs) {
        db.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        db.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        db.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        db.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                          VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        db.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                           VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        db.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        db.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        db.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        db.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    }
    // Multisampled, the resolve writes the single-sampled depth after the
    // scene: it waits only for the previous frame's resolve.
    dbs[0].image = multisampled ? t.ms_depth : t.depth;
    dbs[1].image = t.depth;
    VkDependencyInfo dep{};
    dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dep.imageMemoryBarrierCount = multisampled ? 2 : 1;
    dep.pImageMemoryBarriers = dbs.data();
    vkCmdPipelineBarrier2(cmd, &dep);

    if (multisampled) {
        begin_pass(cmd, t.ms_view, t.extent, t.ms_depth_view, VK_ATTACHMENT_LOAD_OP_CLEAR,
                   VK_ATTACHMENT_STORE_OP_STORE);
    } else {  // kept for later passes, as the resolve's is
        begin_pass(cmd, t.view, t.extent, t.depth_view, VK_ATTACHMENT_LOAD_OP_CLEAR,
                   VK_ATTACHMENT_STORE_OP_STORE);
    }
    vkCmdDraw(cmd, 3, 1, 0, 0);
    if (terrain) {
        terrain->draw(cmd, static_cast<std::uint32_t>(slot), static_cast<std::uint32_t>(i), view,
                      lighting_offset(slot, i));
    }
    vkCmdEndRendering(cmd);
    if (multisampled) {
        // The weighted resolve (D-028) into the HDR target, and the nearest
        // sample's depth into its depth: the multisampled targets now read,
        // the HDR target written (after the previous frame's tone-map read).
        std::array<VkImageMemoryBarrier2, 3> rbs{};
        for (VkImageMemoryBarrier2& b : rbs) {
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        }
        rbs[0].image = t.ms;
        rbs[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        rbs[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        rbs[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        rbs[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        rbs[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        rbs[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        rbs[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        rbs[1].image = t.ms_depth;
        rbs[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        rbs[1].srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        rbs[1].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        rbs[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        rbs[1].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        rbs[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        rbs[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        rbs[2].image = t.image;
        rbs[2].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        rbs[2].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        rbs[2].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        rbs[2].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        rbs[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        rbs[2].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkDependencyInfo rdep{};
        rdep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        rdep.imageMemoryBarrierCount = static_cast<std::uint32_t>(rbs.size());
        rdep.pImageMemoryBarriers = rbs.data();
        vkCmdPipelineBarrier2(cmd, &rdep);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolve_layout, 0, 1,
                                &t.resolve_set, 0, nullptr);
        ResolvePush rp;
        rp.samples = static_cast<std::uint32_t>(samples);
        vkCmdPushConstants(cmd, resolve_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(rp), &rp);
        draw_fullscreen(cmd, t.view, t.extent, t.depth_view);
    }
    // The stars (step 4), single-sampled into the resolved target, tested
    // against the resolved depth: after the writes to both.
    std::array<VkImageMemoryBarrier2, 2> sbs{};
    for (VkImageMemoryBarrier2& b : sbs) {
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    }
    sbs[0].image = t.image;
    sbs[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    sbs[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    sbs[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    sbs[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    sbs[0].dstAccessMask =
        VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    sbs[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sbs[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sbs[1].image = t.depth;
    sbs[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    sbs[1].srcStageMask =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    sbs[1].srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    sbs[1].dstStageMask =
        VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    sbs[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    sbs[1].oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    sbs[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    VkDependencyInfo sdep{};
    sdep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    sdep.imageMemoryBarrierCount = static_cast<std::uint32_t>(sbs.size());
    sdep.pImageMemoryBarriers = sbs.data();
    vkCmdPipelineBarrier2(cmd, &sdep);
    // The stars only test the depth: it isn't stored again (STORE would be
    // a write the barrier above doesn't order).
    begin_pass(cmd, t.view, t.extent, t.depth_view, VK_ATTACHMENT_LOAD_OP_LOAD,
               VK_ATTACHMENT_STORE_OP_NONE, VK_ATTACHMENT_LOAD_OP_LOAD);
    stars->record(cmd, view, sky, t.extent, lighting_offset(slot, i));
    vkCmdEndRendering(cmd);
    transition(cmd, t.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

std::uint32_t Renderer::Impl::draw(const DrawRequest& request) {
    const auto started = now();  // fovea::now(), the monotonic clock
    const std::size_t slot = frame_number % kFramesInFlight;
    Frame& f = begin_frame();
    if (terrain && request.scene) {
        // Tiles the paging thread has ready, and slots of frames done.
        std::uint64_t completed = 0;
        check(vkGetSemaphoreCounterValue(device.device, timeline, &completed),
              "vkGetSemaphoreCounterValue");
        terrain->begin_frame(frame_number + 1, completed);
    }

    // Acquire an image from every swapchain that has a drawable size.
    std::fill(image_index.begin(), image_index.end(), UINT32_MAX);
    for (std::size_t i = 0; i < swapchains.size(); ++i) {
        Swapchain& s = swapchains[i];
        const VkExtent2D size = surface_extent(physical.physical_device, s.surface, s.window_size);
        if (size.width == 0 || size.height == 0) {
            continue;  // minimized: nothing to draw into
        }
        // A resize the driver doesn't report as out of date. The extent
        // compared is the one a rebuild would use, so a rebuild settles it.
        if (s.chain.swapchain != VK_NULL_HANDLE &&
            (size.width != s.chain.extent.width || size.height != s.chain.extent.height)) {
            s.needs_rebuild = true;
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (s.needs_rebuild) {
                check(vkDeviceWaitIdle(device.device), "vkDeviceWaitIdle");
                build_swapchain(s);
                if (s.chain.swapchain == VK_NULL_HANDLE) {
                    break;  // still not drawable
                }
            }
            const VkResult r =
                vkAcquireNextImageKHR(device.device, s.chain.swapchain, UINT64_MAX, f.acquired[i],
                                      VK_NULL_HANDLE, &image_index[i]);
            if (r == VK_ERROR_OUT_OF_DATE_KHR) {
                s.needs_rebuild = true;
                image_index[i] = UINT32_MAX;
                continue;
            }
            if (r == VK_SUBOPTIMAL_KHR) {
                s.needs_rebuild = true;  // after this frame's present
            } else {
                check(r, "vkAcquireNextImageKHR");
            }
            break;
        }
    }

    const auto acquired = now();
    check(vkResetCommandBuffer(f.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(f.cmd, &begin), "vkBeginCommandBuffer");

    // Two timestamps per output, around that output's work. Every output's
    // pair is written, drawn or not, so every query has a result.
    const std::uint32_t queries_per_slot = output_total() * 2;
    const std::uint32_t first_query = static_cast<std::uint32_t>(slot) * queries_per_slot;
    if (timestamps != VK_NULL_HANDLE) {
        vkCmdResetQueryPool(f.cmd, timestamps, first_query, queries_per_slot);
    }
    const auto stamp = [&](std::size_t output, std::uint32_t which) {
        if (timestamps != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp2(f.cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timestamps,
                                 first_query + static_cast<std::uint32_t>(output) * 2 + which);
        }
    };

    // The sky's passes, before the first output's scene, inside its timing:
    // the first output's GPU time includes them.
    bool sky_recorded = false;
    const auto sky_first = [&] {
        if (request.scene && !sky_recorded) {
            record_sky(f.cmd, slot, request.views);
            sky_recorded = true;
        }
    };

    // The stage that first touches an acquired image: the clear's transfer,
    // or the tone-map pass's colour output. The acquire semaphore is waited
    // on there, and the first barrier's source scope starts there too.
    const VkPipelineStageFlags2 first_use = request.scene
                                                ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT
                                                : VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const auto output_colour = [&](std::size_t i) {
        return i < request.colours.size() ? request.colours[i] : Rgb8{};
    };
    const auto tonemap_into = [&](VkImageView target, VkExtent2D extent, VkFormat format,
                                  std::size_t i) {
        vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemap_pipeline(format));
        vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonemap_layout, 0, 1,
                                &hdr[i].set, 0, nullptr);
        TonemapPush push;
        push.exposure = i < request.views.size() ? request.views[i].exposure : 1.0F;
        push.encode_srgb = is_srgb(format) ? 0U : 1U;
        vkCmdPushConstants(f.cmd, tonemap_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push),
                           &push);
        draw_fullscreen(f.cmd, target, extent);
    };

    std::uint32_t wait_count = 0;
    for (std::size_t i = 0; i < swapchains.size(); ++i) {
        stamp(i, 0);
        sky_first();
        if (image_index[i] == UINT32_MAX) {
            stamp(i, 1);
            continue;
        }
        Swapchain& s = swapchains[i];
        VkImage image = s.images[image_index[i]];  // a handle, copied
        if (request.scene) {
            record_scene(f.cmd, slot, i, i < request.views.size() ? request.views[i] : SceneView{});
            transition(f.cmd, image, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, first_use, 0,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            tonemap_into(s.views[image_index[i]], s.chain.extent, s.chain.image_format, i);
            // Only while the swapchain has the format ImGui's pipeline was
            // made for (a rebuild could, rarely, change it).
            if (overlay && request.overlay != nullptr && i == overlay_output &&
                s.chain.image_format == overlay_format) {
                if (!overlay_frame(f.cmd, s.views[image_index[i]], s.chain.extent,
                                   *request.overlay)) {
                    ++overlay_skipped;
                }
            }
            transition(f.cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
        } else {
            transition(f.cmd, image, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, first_use, 0,
                       VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            const VkClearColorValue c =
                clear_value(output_colour(i), is_srgb(s.chain.image_format));
            vkCmdClearColorImage(f.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &all);
            transition(f.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
        }
        VkSemaphoreSubmitInfo& w = waits[wait_count];
        w = {};
        w.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        w.semaphore = f.acquired[i];
        w.stageMask = first_use;
        VkSemaphoreSubmitInfo& sig = signals[wait_count];
        sig = {};
        sig.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        sig.semaphore = s.rendered[image_index[i]];
        sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        ++wait_count;
        stamp(i, 1);
    }

    if (!offscreen.empty()) {
        // The offscreen images and readback buffers are shared by both frames
        // in flight: order this frame's transfers after the previous frame's
        // (its copy reads the image and writes the buffer).
        memory_barrier(f.cmd, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }
    for (std::size_t i = 0; i < offscreen.size(); ++i) {
        const OffscreenImage& o = offscreen[i];
        stamp(swapchains.size() + i, 0);
        sky_first();
        // After the previous frame's copy out of this image (write after read).
        if (request.scene) {
            record_scene(f.cmd, slot, i, i < request.views.size() ? request.views[i] : SceneView{});
            transition(f.cmd, o.image, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            tonemap_into(o.view, {o.width, o.height}, kOffscreenFormat, i);
            if (overlay && request.overlay != nullptr && swapchains.size() + i == overlay_output) {
                if (!overlay_frame(f.cmd, o.view, {o.width, o.height}, *request.overlay)) {
                    ++overlay_skipped;
                }
            }
            transition(f.cmd, o.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT);
        } else {
            transition(f.cmd, o.image, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                       0, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            const VkClearColorValue c = clear_value(output_colour(i), false);
            vkCmdClearColorImage(f.cmd, o.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &all);
            transition(f.cmd, o.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT);
        }
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {o.width, o.height, 1};
        vkCmdCopyImageToBuffer(f.cmd, o.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.readback, 1,
                               &copy);
        stamp(swapchains.size() + i, 1);
    }
    if (!offscreen.empty()) {
        // Make the copies visible to the host once the fence signals.
        memory_barrier(f.cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    }
    check(vkEndCommandBuffer(f.cmd), "vkEndCommandBuffer");

    // Terrain uploads first (the paging thread recorded them: tiles and
    // models), then the frame: in one submit, so the frame's draws see the
    // uploaded tiles. Room for every upload a frame can take, and the frame.
    std::array<VkCommandBufferSubmitInfo, kMaxUploadsInAFrame + 1> cmds{};
    std::uint32_t cmd_count = 0;
    if (terrain && request.scene) {
        const auto uploads = terrain->uploads();
        // Never past the array, whatever the terrain hands over.
        if (uploads.size() >= cmds.size()) {
            fail("terrain: " + std::to_string(uploads.size()) + " uploads in a frame, room for " +
                 std::to_string(cmds.size() - 1));
        }
        for (const VkCommandBufferSubmitInfo& u : uploads) {
            cmds[cmd_count++] = u;
        }
    }
    cmds[cmd_count].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmds[cmd_count].commandBuffer = f.cmd;
    ++cmd_count;
    // And the timeline: frame_number + 1 once this frame is done.
    VkSemaphoreSubmitInfo& tl = signals[wait_count];
    tl = {};
    tl.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    tl.semaphore = timeline;
    tl.value = frame_number + 1;
    tl.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = wait_count;
    submit.pWaitSemaphoreInfos = waits.data();
    submit.commandBufferInfoCount = cmd_count;
    submit.pCommandBufferInfos = cmds.data();
    submit.signalSemaphoreInfoCount = wait_count + 1;
    submit.pSignalSemaphoreInfos = signals.data();
    // Reset only now, with the commands recorded: a failure while recording
    // must not leave this frame's fence unsignalled for the next wait.
    check(vkResetFences(device.device, 1, &f.done), "vkResetFences");
    check(vkQueueSubmit2(queue, 1, &submit, f.done), "vkQueueSubmit2");
    slot_frame[slot] = frame_number;
    if (terrain && request.scene) {
        terrain->submitted(frame_number + 1);
    }

    // Every acquired swapchain in one present call: the outputs flip
    // together as far as the driver allows (the display sync test measures
    // how far that is).
    std::uint32_t presenting = 0;
    for (std::size_t i = 0; i < swapchains.size(); ++i) {
        if (image_index[i] != UINT32_MAX) {
            present_chains[presenting] = swapchains[i].chain.swapchain;
            present_indices[presenting] = image_index[i];
            present_waits[presenting] = swapchains[i].rendered[image_index[i]];
            present_which[presenting] = i;
            present_results[presenting] = VK_SUCCESS;
            ++presenting;
        }
    }
    if (presenting > 0) {
        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = presenting;
        present.pWaitSemaphores = present_waits.data();
        present.swapchainCount = presenting;
        present.pSwapchains = present_chains.data();
        present.pImageIndices = present_indices.data();
        present.pResults = present_results.data();
        // Every swapchain gets the frame's id; waits use the first one's.
        VkPresentIdKHR ids{};
        if (present_wait) {
            std::fill_n(present_ids.begin(), presenting, frame_number + 1);
            ids.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
            ids.swapchainCount = presenting;
            ids.pPresentIds = present_ids.data();
            present.pNext = &ids;
        }
        const VkResult r = vkQueuePresentKHR(queue, &present);
        // Swapchain 0's present, if it was queued: what the next wait waits on.
        presented = present_which[0] == 0 &&
                    (present_results[0] == VK_SUCCESS || present_results[0] == VK_SUBOPTIMAL_KHR);
        presented_generation = swapchains[0].generation;
        last_present_id = frame_number + 1;
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_ERROR_OUT_OF_DATE_KHR) {
            check(r, "vkQueuePresentKHR");
        }
        for (std::uint32_t k = 0; k < presenting; ++k) {
            if (present_results[k] == VK_SUBOPTIMAL_KHR ||
                present_results[k] == VK_ERROR_OUT_OF_DATE_KHR) {
                swapchains[present_which[k]].needs_rebuild = true;
            }
        }
    }
    ++frame_number;
    const auto finished = now();
    timing.wait_ms =
        present_wait_ms + std::chrono::duration<double, std::milli>(acquired - started).count();
    present_wait_ms = 0.0;
    timing.work_ms = std::chrono::duration<double, std::milli>(finished - acquired).count();
    return presenting + static_cast<std::uint32_t>(offscreen.size());
}

Renderer::Impl::~Impl() {
    if (imgui != nullptr) {
        if (device.device != VK_NULL_HANDLE) {
            (void)vkDeviceWaitIdle(device.device);
        }
        ImGui_ImplVulkan_Shutdown();
        ImGui::DestroyContext(imgui);
    }
    if (device.device != VK_NULL_HANDLE) {
        (void)vkDeviceWaitIdle(device.device);
        for (Frame& f : frames) {
            for (VkSemaphore s : f.acquired) {
                vkDestroySemaphore(device.device, s, nullptr);
            }
            vkDestroyFence(device.device, f.done, nullptr);
            vkDestroyCommandPool(device.device, f.pool, nullptr);
        }
        for (Swapchain& s : swapchains) {
            destroy_swapchain(s);
        }
        for (OffscreenImage& o : offscreen) {
            vkDestroyImageView(device.device, o.view, nullptr);
            vmaDestroyBuffer(allocator, o.readback, o.readback_memory);
            vmaDestroyImage(allocator, o.image, o.image_memory);
        }
        terrain.reset();
        stars.reset();
        sky_tables.reset();
        shadows.reset();
        vkDestroySemaphore(device.device, timeline, nullptr);
        for (HdrTarget& t : hdr) {
            vkDestroyImageView(device.device, t.view, nullptr);
            vmaDestroyImage(allocator, t.image, t.memory);
            vkDestroyImageView(device.device, t.depth_view, nullptr);
            vmaDestroyImage(allocator, t.depth, t.depth_memory);
            if (t.ms != VK_NULL_HANDLE) {
                vkDestroyImageView(device.device, t.ms_view, nullptr);
                vmaDestroyImage(allocator, t.ms, t.ms_memory);
                vkDestroyImageView(device.device, t.ms_depth_view, nullptr);
                vmaDestroyImage(allocator, t.ms_depth, t.ms_depth_memory);
            }
        }
        vkDestroyQueryPool(device.device, timestamps, nullptr);
        for (const auto& [format, p] : tonemap_pipelines) {
            vkDestroyPipeline(device.device, p, nullptr);
        }
        vkDestroyPipeline(device.device, scene_pipeline, nullptr);
        vkDestroyPipeline(device.device, resolve_pipeline, nullptr);
        vkDestroyPipelineLayout(device.device, resolve_layout, nullptr);
        vkDestroyDescriptorSetLayout(device.device, resolve_set_layout, nullptr);
        vkDestroyShaderModule(device.device, fs_resolve, nullptr);
        vkDestroyPipelineLayout(device.device, tonemap_layout, nullptr);
        vkDestroyPipelineLayout(device.device, scene_layout, nullptr);
        vkDestroyDescriptorPool(device.device, descriptor_pool, nullptr);
        vkDestroyDescriptorSetLayout(device.device, frame_layout, nullptr);
        if (lighting != VK_NULL_HANDLE) {
            vmaDestroyBuffer(allocator, lighting, lighting_memory);
        }
        vkDestroyDescriptorSetLayout(device.device, set_layout, nullptr);
        vkDestroySampler(device.device, sampler, nullptr);
        vkDestroyShaderModule(device.device, fs_tonemap, nullptr);
        vkDestroyShaderModule(device.device, fs_scene, nullptr);
        vkDestroyShaderModule(device.device, vs_fullscreen, nullptr);
        if (allocator != nullptr) {
            vmaDestroyAllocator(allocator);
        }
        vkb::destroy_device(device);
    }
    if (instance.instance != VK_NULL_HANDLE) {
        for (Swapchain& s : swapchains) {
            if (s.surface != VK_NULL_HANDLE) {
                vkb::destroy_surface(instance, s.surface);
            }
        }
        vkb::destroy_instance(instance);
    }
    if (owns_alive_flag) {
        g_renderer_alive = false;
    }
}

Renderer::Renderer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Renderer::~Renderer() = default;

std::unique_ptr<Renderer> Renderer::for_windows(Platform& platform,
                                                std::span<const WindowOutput> outputs,
                                                const RendererOptions& options) {
    if (outputs.empty()) {
        fail("no windows to render to");
    }
    auto impl = std::make_unique<Impl>();
    impl->claim();
    impl->create_instance(platform.vulkan_instance_extensions(), options.validation);
    std::vector<VkExtent2D> render_extents;
    for (const WindowOutput& o : outputs) {
        Swapchain s;
        s.window = o.window;
        s.present_mode = vk_present_mode(o.present_mode);
        // NOLINTNEXTLINE(performance-no-int-to-ptr): handles cross the platform API as integers
        s.surface = reinterpret_cast<VkSurfaceKHR>(o.window->create_vulkan_surface(
            reinterpret_cast<std::uintptr_t>(impl->instance.instance)));
        const auto size = o.window->pixel_size();
        s.window_size = {size.width, size.height};
        const VkExtent2D extent{o.render_width != 0 ? o.render_width : size.width,
                                o.render_height != 0 ? o.render_height : size.height};
        if (extent.width == 0 || extent.height == 0) {
            fail("a window without a size needs an explicit render size");
        }
        render_extents.push_back(extent);
        impl->swapchains.push_back(std::move(s));
    }
    impl->create_device(true);
    impl->choose_samples(options.msaa);
    impl->frame_period_s = 1.0 / std::max(options.frame_rate_hz, 1.0);
    for (Swapchain& s : impl->swapchains) {
        impl->build_swapchain(s);  // deferred while a window has no size
    }
    impl->create_scene_resources(render_extents);
    impl->size_scratch();
    impl->create_frames();
    impl->create_timestamps();
    return std::unique_ptr<Renderer>(new Renderer(std::move(impl)));
}

std::unique_ptr<Renderer> Renderer::offscreen(std::span<const OffscreenOutput> outputs,
                                              const RendererOptions& options) {
    if (outputs.empty()) {
        fail("no offscreen outputs to render to");
    }
    auto impl = std::make_unique<Impl>();
    impl->claim();
    impl->create_instance({}, options.validation);
    impl->create_device(false);
    impl->choose_samples(options.msaa);
    impl->frame_period_s = 1.0 / std::max(options.frame_rate_hz, 1.0);
    std::vector<VkExtent2D> render_extents;
    for (const OffscreenOutput& o : outputs) {
        impl->create_offscreen(o);
        render_extents.push_back({o.width, o.height});
    }
    impl->create_scene_resources(render_extents);
    impl->size_scratch();
    impl->create_frames();
    impl->create_timestamps();
    return std::unique_ptr<Renderer>(new Renderer(std::move(impl)));
}

const GpuInfo& Renderer::gpu() const {
    return impl_->info;
}

std::uint32_t Renderer::output_count() const {
    return static_cast<std::uint32_t>(impl_->swapchains.size() + impl_->offscreen.size());
}

std::uint32_t Renderer::draw_clear_frame(std::span<const Rgb8> colours) {
    return impl_->draw({colours, {}, false});
}

std::uint32_t Renderer::draw_scene_frame(std::span<const SceneView> views,
                                         const OverlayContent* overlay) {
    return impl_->draw({{}, views, true, overlay});
}

void Renderer::set_sky(const SkyBodies& sky) {
    impl_->sky = sky;
}

void Renderer::set_visibility(double visibility_m) {
    if (impl_->sky_tables) {
        impl_->sky_tables->set_visibility(visibility_m);
    }
}

void Renderer::set_weather(std::span<const WeatherLayer> layers) {
    impl_->weather.clear();
    for (const WeatherLayer& l : layers) {
        (void)impl_->weather.push_back(l);
    }
}

SkyReadback Renderer::read_back_sky(std::uint32_t output) {
    if (!impl_->sky_tables || !impl_->sky_tables->started()) {
        fail("read_back_sky: no scene has been drawn yet");
    }
    return impl_->sky_tables->read_back(output);
}

TerrainLoad Renderer::load_terrain(std::unique_ptr<TerrainDatabase> database,
                                   const TerrainOptions& options) {
    Impl& m = *impl_;
    if (m.terrain) {
        fail("load_terrain: terrain is already loaded");
    }
    check(vkDeviceWaitIdle(m.device.device), "vkDeviceWaitIdle");
    // The shadow maps (step 5), into the frame set (bindings 6 and 7) and,
    // through the GpuContext, the terrain's own set.
    m.shadows = std::make_unique<ShadowMaps>(GpuContext{m.gpu_context()},
                                             static_cast<std::uint32_t>(m.hdr.size()));
    const VkDescriptorImageInfo shadow_info{VK_NULL_HANDLE, m.shadows->maps(),
                                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkDescriptorImageInfo shadow_sampler_info{m.shadows->sampler(), VK_NULL_HANDLE,
                                                    VK_IMAGE_LAYOUT_UNDEFINED};
    std::array<VkWriteDescriptorSet, 2> sws{};
    for (std::uint32_t w = 0; w < sws.size(); ++w) {
        sws[w].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        sws[w].dstSet = m.frame_set;
        sws[w].dstBinding = 6 + w;
        sws[w].descriptorCount = 1;
    }
    sws[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sws[0].pImageInfo = &shadow_info;
    sws[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    sws[1].pImageInfo = &shadow_sampler_info;
    vkUpdateDescriptorSets(m.device.device, static_cast<std::uint32_t>(sws.size()), sws.data(), 0,
                           nullptr);
    const GpuContext gpu = m.gpu_context();
    const std::array<VkShaderModule, 7> modules = {
        m.make_module(kVsTerrain),    m.make_module(kFsTerrain), m.make_module(kVsTerrainShadow),
        m.make_module(kVsModel),      m.make_module(kFsModel),   m.make_module(kVsModelShadow),
        m.make_module(kFsModelShadow)};
    const TerrainShaders shaders{modules[0], modules[1], modules[2],
                                 ModelShaders{modules[3], modules[4], modules[5], modules[6]}};
    const auto release = [&] {
        for (VkShaderModule s : modules) {
            vkDestroyShaderModule(m.device.device, s, nullptr);
        }
    };
    TerrainLoad report;
    try {
        m.terrain = TerrainGpu::load(gpu, std::move(database), options, kFramesInFlight,
                                     m.output_total(), shaders, kHdrFormat, kDepthFormat, report);
    } catch (...) {
        release();
        throw;
    }
    release();
    return report;
}

std::vector<std::string> Renderer::terrain_problems() const {
    return impl_->terrain ? impl_->terrain->problems() : std::vector<std::string>{};
}

const TerrainStats& Renderer::terrain_stats() const {
    static const TerrainStats none;
    return impl_->terrain ? impl_->terrain->stats() : none;
}

void Renderer::enable_overlay(std::uint32_t output) {
    Impl& m = *impl_;
    if (output >= m.output_total()) {
        fail("enable_overlay: there is no output " + std::to_string(output));
    }
    if (m.overlay) {
        return;
    }
    std::uint32_t image_count = 3;
    if (output < m.swapchains.size()) {
        // The format a swapchain gets, if it isn't built yet (a window that
        // starts minimized builds it later).
        const Swapchain& s = m.swapchains[output];
        m.overlay_format =
            s.chain.swapchain != VK_NULL_HANDLE ? s.chain.image_format : kSwapchainFormat;
        image_count = std::max<std::uint32_t>(2, static_cast<std::uint32_t>(s.images.size()));
    } else {
        m.overlay_format = kOffscreenFormat;
    }
    ImGui::SetAllocatorFunctions(&imgui_alloc, &imgui_free);
    ImGuiContext* context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // no files: nothing to read or write while drawing
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();

    ImGui_ImplVulkan_InitInfo init{};
    init.ApiVersion = VK_API_VERSION_1_3;
    init.Instance = m.instance.instance;
    init.PhysicalDevice = m.physical.physical_device;
    init.Device = m.device.device;
    init.QueueFamily = m.queue_family;
    init.Queue = m.queue;
    init.DescriptorPoolSize = 8;  // the backend's own pool, for its font texture
    init.MinImageCount = 2;
    init.ImageCount = image_count;
    init.UseDynamicRendering = true;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    init.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &m.overlay_format;
    if (!ImGui_ImplVulkan_Init(&init)) {
        ImGui::DestroyContext(context);  // nothing to shut down: the backend never started
        fail("enable_overlay: cannot start ImGui's Vulkan backend");
    }
    m.imgui = context;

    // Upload the font now, with every glyph the overlay can show (printable
    // ASCII). ImGui uploads textures from inside drawing with a submit and
    // vkQueueWaitIdle; done here, the render thread never waits for it.
    ImGui_ImplVulkan_NewFrame();
    io.DisplaySize = ImVec2(1024.0F, 1024.0F);
    io.DeltaTime = 1.0F / 60.0F;
    ImGui::NewFrame();
    std::array<char, 96> ascii{};
    for (std::size_t c = 0; c < 95; ++c) {
        ascii[c] = static_cast<char>(' ' + c);
    }
    ImGui::Begin("warm-up");
    ImGui::TextUnformatted(ascii.data());
    ImGui::End();
    ImGui::Render();
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->Status != ImTextureStatus_OK) {
            ImGui_ImplVulkan_UpdateTexture(tex);
        }
    }
    m.overlay = true;
    m.overlay_output = output;
}

std::uint64_t Renderer::overlay_skipped_frames() const {
    return impl_->overlay_skipped;
}

bool Renderer::overlay_ready() const {
    if (!impl_->overlay) {
        return false;
    }
    const auto& textures = ImGui::GetPlatformIO().Textures;
    return std::all_of(textures.begin(), textures.end(),
                       [](const ImTextureData* tex) { return tex->Status == ImTextureStatus_OK; });
}

std::vector<std::uint8_t> Renderer::read_back_rgba(std::uint32_t output) {
    if (output >= impl_->offscreen.size()) {
        fail("read_back_rgba: output " + std::to_string(output) + " is not an offscreen output");
    }
    if (impl_->frame_number == 0) {
        fail("read_back_rgba: nothing has been drawn yet");
    }
    // The last frame's fence.
    const std::size_t last = (impl_->frame_number - 1) % kFramesInFlight;
    check(vkWaitForFences(impl_->device.device, 1, &impl_->frames[last].done, VK_TRUE, UINT64_MAX),
          "vkWaitForFences");
    const OffscreenImage& o = impl_->offscreen[output];
    const std::size_t bytes = static_cast<std::size_t>(o.width) * o.height * 4;
    check(vmaInvalidateAllocation(impl_->allocator, o.readback_memory, 0, VK_WHOLE_SIZE),
          "vmaInvalidateAllocation");
    void* mapped = nullptr;
    check(vmaMapMemory(impl_->allocator, o.readback_memory, &mapped), "vmaMapMemory");
    std::vector<std::uint8_t> pixels(bytes);
    std::memcpy(pixels.data(), mapped, bytes);
    vmaUnmapMemory(impl_->allocator, o.readback_memory);
    return pixels;
}

const FrameTiming& Renderer::last_timing() const {
    return impl_->timing;
}

double Renderer::wait_for_last_present(double timeout_ms) {
    Impl& m = *impl_;
    if (!m.present_wait || !m.presented || m.swapchains.empty() ||
        m.swapchains[0].generation != m.presented_generation ||
        m.swapchains[0].chain.swapchain == VK_NULL_HANDLE) {
        return 0.0;  // unsupported, nothing queued, or that swapchain was rebuilt since
    }
    const auto started = now();
    // Bounded: a covered window may not be shown, and must not hold back the
    // others for long.
    const auto timeout_ns = static_cast<std::uint64_t>(std::max(timeout_ms, 0.0) * 1e6);
    const VkResult r = vkWaitForPresentKHR(m.device.device, m.swapchains[0].chain.swapchain,
                                           m.last_present_id, timeout_ns);
    if (r != VK_SUCCESS && r != VK_TIMEOUT && r != VK_SUBOPTIMAL_KHR &&
        r != VK_ERROR_OUT_OF_DATE_KHR) {
        check(r, "vkWaitForPresentKHR");
    }
    m.present_wait_ms = std::chrono::duration<double, std::milli>(now() - started).count();
    return m.present_wait_ms;
}

std::uint64_t Renderer::frames_drawn() const {
    return impl_->frame_number;
}

bool Renderer::has_present_wait() const {
    return impl_->present_wait;
}

bool Renderer::paced_by_display() const {
    return std::any_of(impl_->swapchains.begin(), impl_->swapchains.end(), [](const Swapchain& s) {
        return s.chain.swapchain != VK_NULL_HANDLE &&
               s.chain.present_mode == VK_PRESENT_MODE_FIFO_KHR;
    });
}

std::uint64_t Renderer::validation_messages() const {
    return impl_->messages.load();
}

}  // namespace fovea
