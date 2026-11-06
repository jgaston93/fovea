// The Vulkan backend: instance and device (vk-bootstrap, volk), memory (VMA),
// and the two presenters behind Renderer.
//
// Frames: kFramesInFlight frames are recorded ahead, each with its own
// command buffer, fence and per-output acquire semaphores. Every swapchain
// image has its own "rendered" semaphore, since a present may still be
// using it when the next frame's fence has already signalled.

#include <volk.h>

#include <VkBootstrap.h>
#include <vk_mem_alloc.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fovea/render/renderer.hpp"

namespace fovea {

namespace {

constexpr std::uint32_t kFramesInFlight = 2;
// UNORM, so a clear to 200 shows 200 on screen (the identify check reads
// exact colours). The scene's tone-mapped output decides this again later.
constexpr VkFormat kSwapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
constexpr VkFormat kOffscreenFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

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

VkClearColorValue clear_value(Rgb8 c) {
    VkClearColorValue v{};
    v.float32[0] = static_cast<float>(c.r) / 255.0F;
    v.float32[1] = static_cast<float>(c.g) / 255.0F;
    v.float32[2] = static_cast<float>(c.b) / 255.0F;
    v.float32[3] = 1.0F;
    return v;
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

struct Swapchain {
    const Window* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    vkb::Swapchain chain;
    std::vector<VkImage> images;
    std::vector<VkSemaphore> rendered;  // one per image
    bool needs_rebuild = false;
};

struct OffscreenImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation image_memory = nullptr;
    VkBuffer readback = VK_NULL_HANDLE;
    VmaAllocation readback_memory = nullptr;
};

struct Frame {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkFence done = VK_NULL_HANDLE;
    std::vector<VkSemaphore> acquired;  // one per swapchain
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

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl();

    void create_instance(const std::vector<std::string>& extensions, Validation validation);
    void create_device(bool present);
    void create_frames();
    void build_swapchain(Swapchain& s) const;
    void destroy_swapchain(Swapchain& s) const;
    void create_offscreen(OffscreenOutput o);
    Frame& begin_frame();
    void draw(std::span<const Rgb8> colours);
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
        builder.enable_validation_layers(true)
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
    auto chosen = selector.select();
    if (!chosen) {
        fail("no GPU with Vulkan 1.3 and the features Fovea needs: " + chosen.error().message());
    }
    physical = chosen.value();

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

void Renderer::Impl::build_swapchain(Swapchain& s) const {
    const auto size = s.window->pixel_size();
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
    for (VkSemaphore& r : s.rendered) {
        check(vkCreateSemaphore(device.device, &sem, nullptr, &r), "vkCreateSemaphore");
    }
    s.needs_rebuild = false;
}

void Renderer::Impl::destroy_swapchain(Swapchain& s) const {
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

Frame& Renderer::Impl::begin_frame() {
    Frame& f = frames[frame_number % kFramesInFlight];
    check(vkWaitForFences(device.device, 1, &f.done, VK_TRUE, UINT64_MAX), "vkWaitForFences");
    return f;
}

void Renderer::Impl::draw(std::span<const Rgb8> colours) {
    Frame& f = begin_frame();

    // Acquire an image from every swapchain that has a drawable size.
    std::vector<std::uint32_t> image_index(swapchains.size(), UINT32_MAX);
    for (std::size_t i = 0; i < swapchains.size(); ++i) {
        Swapchain& s = swapchains[i];
        const auto size = s.window->pixel_size();
        if (size.width == 0 || size.height == 0) {
            continue;  // minimized
        }
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (s.needs_rebuild) {
                check(vkDeviceWaitIdle(device.device), "vkDeviceWaitIdle");
                build_swapchain(s);
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

    check(vkResetFences(device.device, 1, &f.done), "vkResetFences");
    check(vkResetCommandBuffer(f.cmd, 0), "vkResetCommandBuffer");
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(f.cmd, &begin), "vkBeginCommandBuffer");

    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    std::vector<VkSemaphoreSubmitInfo> waits;
    std::vector<VkSemaphoreSubmitInfo> signals;
    for (std::size_t i = 0; i < swapchains.size(); ++i) {
        if (image_index[i] == UINT32_MAX) {
            continue;
        }
        Swapchain& s = swapchains[i];
        VkImage image = s.images[image_index[i]];  // a handle, copied
        // The acquire semaphore is waited on at the transfer stage, so the
        // barrier's source scope starts there too.
        transition(f.cmd, image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue c = clear_value(i < colours.size() ? colours[i] : Rgb8{});
        vkCmdClearColorImage(f.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &all);
        transition(f.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
        VkSemaphoreSubmitInfo w{};
        w.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        w.semaphore = f.acquired[i];
        w.stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        waits.push_back(w);
        VkSemaphoreSubmitInfo sig{};
        sig.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
        sig.semaphore = s.rendered[image_index[i]];
        sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals.push_back(sig);
    }
    for (std::size_t i = 0; i < offscreen.size(); ++i) {
        const OffscreenImage& o = offscreen[i];
        transition(f.cmd, o.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue c = clear_value(i < colours.size() ? colours[i] : Rgb8{});
        vkCmdClearColorImage(f.cmd, o.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &all);
        transition(f.cmd, o.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                   VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                   VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {o.width, o.height, 1};
        vkCmdCopyImageToBuffer(f.cmd, o.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, o.readback, 1,
                               &copy);
    }
    if (!offscreen.empty()) {
        // Make the copies visible to the host once the fence signals.
        VkMemoryBarrier2 host{};
        host.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        host.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        VkDependencyInfo d{};
        d.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        d.memoryBarrierCount = 1;
        d.pMemoryBarriers = &host;
        vkCmdPipelineBarrier2(f.cmd, &d);
    }
    check(vkEndCommandBuffer(f.cmd), "vkEndCommandBuffer");

    VkCommandBufferSubmitInfo cmd{};
    cmd.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    cmd.commandBuffer = f.cmd;
    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = static_cast<std::uint32_t>(waits.size());
    submit.pWaitSemaphoreInfos = waits.data();
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmd;
    submit.signalSemaphoreInfoCount = static_cast<std::uint32_t>(signals.size());
    submit.pSignalSemaphoreInfos = signals.data();
    check(vkQueueSubmit2(queue, 1, &submit, f.done), "vkQueueSubmit2");

    // Every acquired swapchain in one present call: the outputs flip
    // together as far as the driver allows (the display sync test measures
    // how far that is).
    std::vector<VkSwapchainKHR> chains;
    std::vector<std::uint32_t> indices;
    std::vector<VkSemaphore> rendered;
    std::vector<std::size_t> which;
    for (std::size_t i = 0; i < swapchains.size(); ++i) {
        if (image_index[i] != UINT32_MAX) {
            chains.push_back(swapchains[i].chain.swapchain);
            indices.push_back(image_index[i]);
            rendered.push_back(swapchains[i].rendered[image_index[i]]);
            which.push_back(i);
        }
    }
    if (!chains.empty()) {
        std::vector<VkResult> results(chains.size(), VK_SUCCESS);
        VkPresentInfoKHR present{};
        present.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        present.waitSemaphoreCount = static_cast<std::uint32_t>(rendered.size());
        present.pWaitSemaphores = rendered.data();
        present.swapchainCount = static_cast<std::uint32_t>(chains.size());
        present.pSwapchains = chains.data();
        present.pImageIndices = indices.data();
        present.pResults = results.data();
        const VkResult r = vkQueuePresentKHR(queue, &present);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR && r != VK_ERROR_OUT_OF_DATE_KHR) {
            check(r, "vkQueuePresentKHR");
        }
        for (std::size_t k = 0; k < results.size(); ++k) {
            if (results[k] == VK_SUBOPTIMAL_KHR || results[k] == VK_ERROR_OUT_OF_DATE_KHR) {
                swapchains[which[k]].needs_rebuild = true;
            }
        }
    }
    ++frame_number;
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
            vmaDestroyBuffer(allocator, o.readback, o.readback_memory);
            vmaDestroyImage(allocator, o.image, o.image_memory);
        }
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
    impl->create_instance(platform.vulkan_instance_extensions(), options.validation);
    for (const WindowOutput& o : outputs) {
        Swapchain s;
        s.window = o.window;
        s.present_mode = vk_present_mode(o.present_mode);
        // NOLINTNEXTLINE(performance-no-int-to-ptr): handles cross the platform API as integers
        s.surface = reinterpret_cast<VkSurfaceKHR>(o.window->create_vulkan_surface(
            reinterpret_cast<std::uintptr_t>(impl->instance.instance)));
        impl->swapchains.push_back(std::move(s));
    }
    impl->create_device(true);
    for (Swapchain& s : impl->swapchains) {
        impl->build_swapchain(s);
    }
    impl->create_frames();
    return std::unique_ptr<Renderer>(new Renderer(std::move(impl)));
}

std::unique_ptr<Renderer> Renderer::offscreen(std::span<const OffscreenOutput> outputs,
                                              const RendererOptions& options) {
    if (outputs.empty()) {
        fail("no offscreen outputs to render to");
    }
    auto impl = std::make_unique<Impl>();
    impl->create_instance({}, options.validation);
    impl->create_device(false);
    for (const OffscreenOutput& o : outputs) {
        impl->create_offscreen(o);
    }
    impl->create_frames();
    return std::unique_ptr<Renderer>(new Renderer(std::move(impl)));
}

const GpuInfo& Renderer::gpu() const {
    return impl_->info;
}

std::uint32_t Renderer::output_count() const {
    return static_cast<std::uint32_t>(impl_->swapchains.size() + impl_->offscreen.size());
}

void Renderer::draw_clear_frame(std::span<const Rgb8> colours) {
    impl_->draw(colours);
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
