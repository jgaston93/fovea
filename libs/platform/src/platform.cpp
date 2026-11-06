#include "fovea/platform/platform.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <utility>

namespace fovea {

namespace {

// SDL's video subsystem is process-wide: one Platform at a time.
std::atomic<bool> g_platform_alive{false};

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what + ": " + SDL_GetError());
}

SDL_Window* sdl(void* handle) {
    return static_cast<SDL_Window*>(handle);
}

EventSummary handle(const SDL_Event& e, EventSummary summary) {
    switch (static_cast<SDL_EventType>(e.type)) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            summary.quit = true;
            break;
        case SDL_EVENT_KEY_DOWN:
            summary.quit = summary.quit || e.key.key == SDLK_ESCAPE;
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
            summary.exposed = true;
            break;
        default:
            break;
    }
    return summary;
}

}  // namespace

Window::~Window() {
    if (handle_ != nullptr) {
        SDL_DestroyWindow(sdl(handle_));
    }
}

Window::Window(Window&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)), display_ids_(std::move(other.display_ids_)) {}

Window& Window::operator=(Window&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            SDL_DestroyWindow(sdl(handle_));
        }
        handle_ = std::exchange(other.handle_, nullptr);
        display_ids_ = std::move(other.display_ids_);
    }
    return *this;
}

Window::PixelSize Window::pixel_size() const {
    int w = 0;
    int h = 0;
    if (handle_ != nullptr) {
        SDL_GetWindowSizeInPixels(sdl(handle_), &w, &h);
    }
    return {static_cast<std::uint32_t>(std::max(w, 0)), static_cast<std::uint32_t>(std::max(h, 0))};
}

bool Window::fill(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (handle_ == nullptr) {
        return false;
    }
    SDL_Surface* surface = SDL_GetWindowSurface(sdl(handle_));
    if (surface == nullptr) {
        return false;
    }
    return SDL_FillSurfaceRect(surface, nullptr, SDL_MapSurfaceRGB(surface, r, g, b)) &&
           SDL_UpdateWindowSurface(sdl(handle_));
}

int Window::current_monitor() const {
    if (handle_ == nullptr) {
        return -1;
    }
    const SDL_DisplayID id = SDL_GetDisplayForWindow(sdl(handle_));
    const auto it = std::find(display_ids_.begin(), display_ids_.end(), id);
    return it == display_ids_.end() ? -1 : static_cast<int>(it - display_ids_.begin());
}

Platform::Platform(const PlatformOptions& options) {
    if (g_platform_alive.exchange(true)) {
        throw std::runtime_error("only one Platform may exist at a time");
    }
    if (!options.video_driver.empty()) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, options.video_driver.c_str());
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        g_platform_alive = false;
        fail("cannot start SDL video");
    }
    try {
        list_monitors();
    } catch (...) {
        // The destructor won't run for a constructor that throws.
        SDL_Quit();
        g_platform_alive = false;
        throw;
    }
}

void Platform::list_monitors() {
    int count = 0;
    const std::unique_ptr<SDL_DisplayID, decltype(&SDL_free)> owned(SDL_GetDisplays(&count),
                                                                    &SDL_free);
    const SDL_DisplayID* ids = owned.get();
    if (ids == nullptr) {
        fail("cannot list the monitors");
    }
    const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
    for (int i = 0; i < count; ++i) {
        MonitorInfo m;
        const char* name = SDL_GetDisplayName(ids[i]);
        m.name = name != nullptr ? name : "";
        SDL_Rect bounds{};
        if (SDL_GetDisplayBounds(ids[i], &bounds)) {
            m.x = bounds.x;
            m.y = bounds.y;
            m.width = static_cast<std::uint32_t>(bounds.w);
            m.height = static_cast<std::uint32_t>(bounds.h);
        }
        if (const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(ids[i])) {
            m.refresh_hz = static_cast<double>(mode->refresh_rate);
            m.pixel_density = static_cast<double>(mode->pixel_density);
        }
        m.primary = ids[i] == primary;
        monitors_.push_back(m);
        display_ids_.push_back(ids[i]);
    }
}

Platform::~Platform() {
    SDL_Quit();
    g_platform_alive = false;
}

Window Platform::create_window(const WindowDesc& desc) {
    if (desc.monitor >= monitors_.size()) {
        throw std::runtime_error("monitor " + std::to_string(desc.monitor) + " does not exist (" +
                                 std::to_string(monitors_.size()) + " connected)");
    }
    const MonitorInfo& m = monitors_[desc.monitor];

    // Created hidden at the monitor's position, so it goes fullscreen on that
    // monitor and not wherever the window manager first puts it.
    const SDL_WindowFlags flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    const int w = static_cast<int>(desc.fullscreen ? m.width : desc.width);
    const int h = static_cast<int>(desc.fullscreen ? m.height : desc.height);
    SDL_Window* window = SDL_CreateWindow(desc.title.c_str(), w, h, flags);
    if (window == nullptr) {
        fail("cannot create window \"" + desc.title + "\"");
    }
    Window result(window);
    result.display_ids_ = display_ids_;

    SDL_SetWindowPosition(window, m.x + (static_cast<int>(m.width) - w) / 2,
                          m.y + (static_cast<int>(m.height) - h) / 2);
    if (desc.fullscreen) {
        // A null mode means fullscreen at the desktop resolution: borderless,
        // no mode switch.
        if (!SDL_SetWindowFullscreenMode(window, nullptr) ||
            !SDL_SetWindowFullscreen(window, true)) {
            fail("cannot make window \"" + desc.title + "\" fullscreen");
        }
    }
    SDL_ShowWindow(window);
    SDL_SyncWindow(window);  // wait until the window system has applied it all
    return result;
}

// Members, not static, so they can only be called while SDL is running.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
EventSummary Platform::poll_events() {
    EventSummary summary;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        summary = handle(e, summary);
    }
    return summary;
}

EventSummary Platform::wait_events(int timeout_ms) {
    EventSummary summary;
    SDL_Event e;
    if (SDL_WaitEventTimeout(&e, timeout_ms)) {
        summary = handle(e, summary);
    }
    const EventSummary rest = poll_events();
    summary.quit = summary.quit || rest.quit;
    summary.exposed = summary.exposed || rest.exposed;
    return summary;
}

// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
std::string Platform::video_driver() const {
    const char* name = SDL_GetCurrentVideoDriver();
    return name != nullptr ? name : "";
}

}  // namespace fovea
