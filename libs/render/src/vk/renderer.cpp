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
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fovea/render/renderer.hpp"
#include "shaders/kFsScene.hpp"
#include "shaders/kFsTonemap.hpp"
#include "shaders/kVsFullscreen.hpp"

namespace fovea {

namespace {

constexpr std::uint32_t kFramesInFlight = 2;
// UNORM, so a clear to 200 shows 200 on screen (the identify check reads
// exact colours); the tone-map pass encodes sRGB itself for UNORM targets.
constexpr VkFormat kSwapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat kOffscreenFormat = VK_FORMAT_R8G8B8A8_UNORM;
// Linear, scene-referred light values (foundational decision 3).
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

// volk keeps the Vulkan entry points in process-wide variables, so a second
// live Renderer would replace the first one's: one at a time.
std::atomic<bool> g_renderer_alive{false};

// The shaders' push-constant blocks (shaders/reference_scene.slang).
struct ScenePush {
    Mat4 clip_to_local{1.0F};
    float eye_height_m = 0.0F;
    float grid_spacing_m = 0.0F;
    float unused[2] = {};  // NOLINT(modernize-avoid-c-arrays): matches the shader's float2
};
static_assert(sizeof(ScenePush) == 80, "must match ScenePush in reference_scene.slang");

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
// size, so the window's pixel size is used, clamped to the surface's limits.
VkExtent2D surface_extent(VkPhysicalDevice gpu, VkSurfaceKHR surface, const Window& window) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps) != VK_SUCCESS) {
        return {0, 0};
    }
    if (caps.currentExtent.width != UINT32_MAX) {
        return caps.currentExtent;
    }
    const auto size = window.pixel_size();
    return {std::clamp(size.width, caps.minImageExtent.width, caps.maxImageExtent.width),
            std::clamp(size.height, caps.minImageExtent.height, caps.maxImageExtent.height)};
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

// Draws the fullscreen triangle into one colour target with dynamic rendering.
void draw_fullscreen(VkCommandBuffer cmd, VkImageView target, VkExtent2D extent) {
    VkRenderingAttachmentInfo colour{};
    colour.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colour.imageView = target;
    colour.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colour.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
    colour.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    info.renderArea = {{0, 0}, extent};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &colour;
    vkCmdBeginRendering(cmd, &info);
    const VkViewport viewport{
        0.0F, 0.0F, static_cast<float>(extent.width), static_cast<float>(extent.height),
        0.0F, 1.0F};
    const VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
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

// The linear HDR image a view renders into before tone mapping.
struct HdrTarget {
    VkExtent2D extent{};
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation memory = nullptr;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;  // the tone-map pass samples it through this
};

struct Swapchain {
    const Window* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D render_extent{};  // 0x0: the window's size
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
    VkPipeline make_pipeline(VkPipelineLayout layout, VkShaderModule fragment,
                             VkFormat format) const;
    VkPipeline tonemap_pipeline(VkFormat format);
    VkImageView make_view(VkImage image, VkFormat format) const;
    void claim();
    void size_scratch();
    Frame& begin_frame();
    std::uint32_t draw(const DrawRequest& request);
    void record_scene(VkCommandBuffer cmd, std::size_t i, const SceneView& view);
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
    // Descriptor indexing for bindless resources (architecture.md, "Render
    // path rules") and timeline semaphores for the paging thread.
    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.timelineSemaphore = VK_TRUE;
    f12.descriptorIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;

    vkb::PhysicalDeviceSelector selector(instance);
    selector.set_minimum_version(1, 3)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
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
}

VkImageView Renderer::Impl::make_view(VkImage image, VkFormat format) const {
    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    check(vkCreateImageView(device.device, &vci, nullptr, &view), "vkCreateImageView");
    return view;
}

void Renderer::Impl::build_swapchain(Swapchain& s) {
    const VkExtent2D size = surface_extent(physical.physical_device, s.surface, *s.window);
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
                                         VkFormat format) const {
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
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
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

    const auto count = static_cast<std::uint32_t>(render_extents.size());
    VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, count};
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = count;
    pci.poolSizeCount = 1;
    pci.pPoolSizes = &pool_size;
    check(vkCreateDescriptorPool(device.device, &pci, nullptr, &descriptor_pool),
          "vkCreateDescriptorPool");

    VkPushConstantRange scene_push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ScenePush)};
    VkPipelineLayoutCreateInfo scene_lci{};
    scene_lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
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

    scene_pipeline = make_pipeline(scene_layout, fs_scene, kHdrFormat);
    (void)tonemap_pipeline(kOffscreenFormat);
    for (const Swapchain& s : swapchains) {
        if (s.chain.swapchain != VK_NULL_HANDLE) {
            (void)tonemap_pipeline(s.chain.image_format);
        }
    }

    for (const VkExtent2D extent : render_extents) {
        HdrTarget t;
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
        hdr.push_back(t);
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
    signals.resize(n);
    present_chains.resize(n);
    present_indices.resize(n);
    present_waits.resize(n);
    present_which.resize(n);
    present_results.resize(n);
}

Frame& Renderer::Impl::begin_frame() {
    Frame& f = frames[frame_number % kFramesInFlight];
    check(vkWaitForFences(device.device, 1, &f.done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    return f;
}

// The scene for output i into its HDR target, ready for the tone-map pass.
void Renderer::Impl::record_scene(VkCommandBuffer cmd, std::size_t i, const SceneView& view) {
    const HdrTarget& t = hdr[i];
    // After the previous frame's tone-map pass read this target (write after
    // read: an execution dependency is enough).
    transition(cmd, t.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, scene_pipeline);
    ScenePush push;
    push.clip_to_local = view.clip_to_local;
    push.eye_height_m = view.eye_height_m;
    push.grid_spacing_m = view.grid_spacing_m;
    vkCmdPushConstants(cmd, scene_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    draw_fullscreen(cmd, t.view, t.extent);
    transition(cmd, t.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
               VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
               VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}

std::uint32_t Renderer::Impl::draw(const DrawRequest& request) {
    Frame& f = begin_frame();

    // Acquire an image from every swapchain that has a drawable size.
    std::fill(image_index.begin(), image_index.end(), UINT32_MAX);
    for (std::size_t i = 0; i < swapchains.size(); ++i) {
        Swapchain& s = swapchains[i];
        const VkExtent2D size = surface_extent(physical.physical_device, s.surface, *s.window);
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

    check(vkResetCommandBuffer(f.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(f.cmd, &begin), "vkBeginCommandBuffer");

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
        if (image_index[i] == UINT32_MAX) {
            continue;
        }
        Swapchain& s = swapchains[i];
        VkImage image = s.images[image_index[i]];  // a handle, copied
        if (request.scene) {
            record_scene(f.cmd, i, i < request.views.size() ? request.views[i] : SceneView{});
            transition(f.cmd, image, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, first_use, 0,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            tonemap_into(s.views[image_index[i]], s.chain.extent, s.chain.image_format, i);
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
        // After the previous frame's copy out of this image (write after read).
        if (request.scene) {
            record_scene(f.cmd, i, i < request.views.size() ? request.views[i] : SceneView{});
            transition(f.cmd, o.image, VK_IMAGE_LAYOUT_UNDEFINED,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0,
                       VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            tonemap_into(o.view, {o.width, o.height}, kOffscreenFormat, i);
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
    }
    if (!offscreen.empty()) {
        // Make the copies visible to the host once the fence signals.
        memory_barrier(f.cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    }
    check(vkEndCommandBuffer(f.cmd), "vkEndCommandBuffer");

    VkCommandBufferSubmitInfo cmd{};
    cmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd.commandBuffer = f.cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = wait_count;
    submit.pWaitSemaphoreInfos = waits.data();
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd;
    submit.signalSemaphoreInfoCount = wait_count;
    submit.pSignalSemaphoreInfos = signals.data();
    // Reset only now, with the commands recorded: a failure while recording
    // must not leave this frame's fence unsignalled for the next wait.
    check(vkResetFences(device.device, 1, &f.done), "vkResetFences");
    check(vkQueueSubmit2(queue, 1, &submit, f.done), "vkQueueSubmit2");

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
        const VkResult r = vkQueuePresentKHR(queue, &present);
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
    return presenting + static_cast<std::uint32_t>(offscreen.size());
}

Renderer::Impl::~Impl() {
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
        for (HdrTarget& t : hdr) {
            vkDestroyImageView(device.device, t.view, nullptr);
            vmaDestroyImage(allocator, t.image, t.memory);
        }
        for (const auto& [format, p] : tonemap_pipelines) {
            vkDestroyPipeline(device.device, p, nullptr);
        }
        vkDestroyPipeline(device.device, scene_pipeline, nullptr);
        vkDestroyPipelineLayout(device.device, tonemap_layout, nullptr);
        vkDestroyPipelineLayout(device.device, scene_layout, nullptr);
        vkDestroyDescriptorPool(device.device, descriptor_pool, nullptr);
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
        render_extents.push_back({o.render_width != 0 ? o.render_width : size.width,
                                  o.render_height != 0 ? o.render_height : size.height});
        impl->swapchains.push_back(std::move(s));
    }
    impl->create_device(true);
    for (Swapchain& s : impl->swapchains) {
        impl->build_swapchain(s);  // deferred while a window has no size
    }
    impl->create_scene_resources(render_extents);
    impl->size_scratch();
    impl->create_frames();
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
    std::vector<VkExtent2D> render_extents;
    for (const OffscreenOutput& o : outputs) {
        impl->create_offscreen(o);
        render_extents.push_back({o.width, o.height});
    }
    impl->create_scene_resources(render_extents);
    impl->size_scratch();
    impl->create_frames();
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

std::uint32_t Renderer::draw_scene_frame(std::span<const SceneView> views) {
    return impl_->draw({{}, views, true});
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

std::uint64_t Renderer::validation_messages() const {
    return impl_->messages.load();
}

}  // namespace fovea
