#pragma once

// The platform layer: displays, windows and input events, on SDL3 (D-001).
// SDL stays behind this header; nothing outside libs/platform sees its
// types. Main thread only, like the windowing systems underneath.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "fovea/platform/monitors.hpp"

namespace fovea {

struct PlatformOptions {
    // SDL video driver to use; empty picks the platform's own. "dummy" gives
    // a fake display with no real windows, for tests on machines without one.
    std::string video_driver;
};

// One window on one monitor.
struct WindowDesc {
    std::string title;
    std::uint32_t monitor = 0;  // index into Platform::monitors()
    // Borderless and covering the whole monitor at its desktop resolution,
    // without a display mode change ("fullscreen desktop").
    bool fullscreen = true;
    // Windowed size, when not fullscreen.
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
};

// Made by Platform::create_window; the Platform must outlive it.
class Window {
public:
    ~Window();
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&& other) noexcept;
    Window& operator=(Window&& other) noexcept;

    // Drawable size in pixels (may differ from the desktop size under
    // display scaling).
    std::uint32_t pixel_width() const;
    std::uint32_t pixel_height() const;
    // The monitor the window is on now, as an index into monitors(), or -1.
    int current_monitor() const;

    // Fills the window with one colour, drawn in software: for identifying
    // which window is on which monitor (fovea --identify). Diagnostic only:
    // it gives the window to SDL's software surface, so the window can't take
    // a Vulkan surface afterwards. Returns false if SDL can't draw it.
    bool fill(std::uint8_t r, std::uint8_t g, std::uint8_t b);

    // The SDL_Window, for the Vulkan surface (step 5) inside libs/platform.
    void* native() const { return handle_; }

private:
    friend class Platform;
    explicit Window(void* handle) : handle_(handle) {}
    void* handle_ = nullptr;
    const std::vector<std::uint32_t>* display_ids_ = nullptr;  // the Platform's
};

struct EventSummary {
    bool quit = false;  // window closed, Esc pressed, or the OS asked to quit
};

class Platform {
public:
    // Starts SDL's video subsystem. Throws std::runtime_error with SDL's
    // message when it can't.
    explicit Platform(const PlatformOptions& options = {});
    ~Platform();
    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;

    // The connected monitors, in SDL's order; config monitor indices refer to
    // this list. The primary one is marked.
    const std::vector<MonitorInfo>& monitors() const { return monitors_; }

    // Throws std::runtime_error if the monitor doesn't exist or SDL fails.
    Window create_window(const WindowDesc& desc);

    // Handles pending events without waiting.
    EventSummary poll_events();
    // Like poll_events, but waits up to timeout_ms for the first event.
    EventSummary wait_events(int timeout_ms);

    // The name of the video driver in use ("windows", "x11", "dummy").
    std::string video_driver() const;

private:
    std::vector<MonitorInfo> monitors_;
    std::vector<std::uint32_t> display_ids_;  // SDL_DisplayID, same order
};

}  // namespace fovea
