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

    struct PixelSize {
        std::uint32_t width = 0;
        std::uint32_t height = 0;
    };
    // Drawable size in pixels (may differ from the desktop size under
    // display scaling). Zero for a moved-from Window.
    PixelSize pixel_size() const;
    // The monitor the window is on now, as an index into monitors(), or -1
    // (also for a moved-from Window).
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
    // The Platform's display ids at creation, mapping SDL's display id to a
    // monitor index (a copy: a few integers, no pointer into the Platform).
    std::vector<std::uint32_t> display_ids_;
};

struct EventSummary {
    bool quit = false;     // window closed, Esc pressed, or the OS asked to quit
    bool exposed = false;  // a window's contents need drawing again
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
    // this list. The primary one is marked. Taken once at startup: monitors
    // plugged in or removed while running are not followed (milestone 1).
    const std::vector<MonitorInfo>& monitors() const { return monitors_; }

    // Throws std::runtime_error if the monitor doesn't exist or SDL fails.
    Window create_window(const WindowDesc& desc);

    // Handles pending events without waiting.
    EventSummary poll_events();
    // Like poll_events, but waits up to timeout_ms for the first event
    // (negative: until one arrives).
    EventSummary wait_events(int timeout_ms);

    // The name of the video driver in use ("windows", "x11", "dummy").
    std::string video_driver() const;

private:
    void list_monitors();

    std::vector<MonitorInfo> monitors_;
    std::vector<std::uint32_t> display_ids_;  // SDL_DisplayID, same order
};

}  // namespace fovea
