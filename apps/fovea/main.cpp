// The runtime IG. Loads the display config, checks it against the connected
// monitors and opens one window per channel, until Esc or a window close.
// Each window shows its channel's view of the reference scene (a ground
// grid, the horizon and the sky).
//
//   fovea [--seconds N] [--stats FILE] [--clear | --identify] <display-config.toml>
//       run; N > 0: exit after N seconds; --stats: write every frame's timing
//       to FILE as CSV; --clear: clear each window to its
//       channel's colour (red, green, blue, ... in config order) on the GPU
//       instead; --identify: fill them with those colours in software (no
//       GPU needed)
//   fovea --list-monitors     print the monitors and exit
//
// An offscreen config renders one frame headless and prints the colour read
// back from the middle of each output.

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fovea/core/config.hpp"
#include "fovea/core/log.hpp"
#include "fovea/core/scene.hpp"
#include "fovea/platform/monitors.hpp"
#include "fovea/platform/platform.hpp"
#include "fovea/render/frame_loop.hpp"
#include "fovea/render/renderer.hpp"
#include "fovea/render/view.hpp"

namespace {

// Each channel's colour, in config order: red, green, blue, then yellow,
// magenta, cyan. tools/check-identify.ps1 reads them from the output.
constexpr std::array<fovea::Rgb8, 6> kColours{
    {{200, 30, 30}, {30, 170, 60}, {40, 80, 220}, {220, 200, 40}, {190, 60, 190}, {40, 190, 200}}};

fovea::Rgb8 colour_of(std::size_t channel) {
    return kColours[channel % kColours.size()];
}

struct Options {
    std::string config;
    double seconds = 0.0;  // 0: until Esc
    std::string stats;     // CSV file for per-frame timing, or empty
    bool clear = false;
    bool identify = false;
    bool list_monitors = false;
};

// Longest --seconds accepted: a week, well inside every duration and timeout
// conversion below (an int of milliseconds overflows after 24.8 days).
constexpr double kMaxSeconds = 7.0 * 24.0 * 3600.0;

void print_monitors(const fovea::Platform& platform) {
    std::printf("%zu monitors (%s):\n", platform.monitors().size(),
                platform.video_driver().c_str());
    for (std::size_t i = 0; i < platform.monitors().size(); ++i) {
        const fovea::MonitorInfo& m = platform.monitors()[i];
        std::printf("  %zu: %ux%u at (%d, %d), %.2f Hz, scale %.2f%s  %s\n", i, m.width, m.height,
                    m.x, m.y, m.refresh_hz, m.pixel_density, m.primary ? ", primary" : "",
                    m.name.c_str());
    }
}

void print_views(const fovea::DisplayConfig& config) {
    std::printf("%zu views at %g Hz:\n", config.channels.size(), config.target_hz);
    for (std::size_t i = 0; i < config.channels.size(); ++i) {
        const auto view = fovea::view_from_channel(config.channels[i], config, i);
        const auto& f = view.projection.frustum;
        std::printf("  %-8s monitor %d, render %ux%u, yaw %g, frustum [%g, %g] x [%g, %g]\n",
                    view.name.c_str(), config.channels[i].monitor, view.render_width,
                    view.render_height, view.pose.rotation.yaw_deg, f.left_deg, f.right_deg,
                    f.bottom_deg, f.top_deg);
    }
}

void print_gpu(const fovea::Renderer& r) {
    const fovea::GpuInfo& g = r.gpu();
    std::printf("GPU: %s (%s, %s, Vulkan %u.%u, validation %s)\n", g.name.c_str(), g.driver.c_str(),
                g.discrete ? "discrete" : "not discrete", g.api_major, g.api_minor,
                g.validation ? "on" : "off");
}

// A positive number of seconds up to kMaxSeconds; the whole text must be the
// number.
std::optional<double> parse_seconds(std::string_view text) {
    double v = 0.0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
    // Written as !(in range) so NaN (from "nan") fails: every comparison with
    // it is false. The De Morgan form would accept it.
    // NOLINTNEXTLINE(readability-simplify-boolean-expr)
    if (ec != std::errc{} || end != text.data() + text.size() || !(v > 0.0 && v <= kMaxSeconds)) {
        return std::nullopt;
    }
    return v;
}

std::optional<Options> parse_args(int argc, char** argv) {
    const std::vector<std::string_view> args(argv + 1, argv + argc);
    Options o;
    if (args.size() == 1 && args[0] == "--list-monitors") {
        o.list_monitors = true;
        return o;
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--seconds" && i + 1 < args.size()) {
            const auto s = parse_seconds(args[++i]);
            if (!s) {
                (void)std::fprintf(stderr,
                                   "fovea: --seconds needs a positive number up to %g (a week), "
                                   "got \"%s\"\n",
                                   kMaxSeconds, std::string(args[i]).c_str());
                return std::nullopt;
            }
            o.seconds = *s;
        } else if (args[i] == "--stats" && i + 1 < args.size()) {
            o.stats = std::string(args[++i]);
        } else if (args[i] == "--identify" && !o.clear) {
            o.identify = true;
        } else if (args[i] == "--clear" && !o.identify) {
            o.clear = true;
        } else if (!args[i].starts_with("--") && o.config.empty()) {
            o.config = std::string(args[i]);
        } else {
            return std::nullopt;
        }
    }
    if (o.config.empty()) {
        return std::nullopt;
    }
    return o;
}

int usage() {
    (void)std::fprintf(
        stderr,
        "usage: fovea [--seconds N] [--stats FILE] [--clear | --identify] <display-config.toml>\n"
        "       fovea --list-monitors\n");
    return 2;
}

void print_summary(const fovea::FrameSummary& s) {
    std::printf(
        "%llu frames, %llu missed; interval mean %.3f ms, max %.3f ms; CPU work mean "
        "%.3f ms, max %.3f ms\n",
        static_cast<unsigned long long>(s.frames), static_cast<unsigned long long>(s.missed),
        s.interval_mean_ms, s.interval_max_ms, s.work_mean_ms, s.work_max_ms);
    for (std::uint32_t i = 0; i < s.gpu_outputs; ++i) {
        std::printf("  GPU, output %u: mean %.3f ms, max %.3f ms\n", i, s.gpu_mean_ms[i],
                    s.gpu_max_ms[i]);
    }
}

int finish(const fovea::Renderer& r) {
    std::printf("%llu validation messages\n",
                static_cast<unsigned long long>(r.validation_messages()));
    return r.validation_messages() == 0 ? 0 : 1;
}

std::vector<fovea::SceneView> scene_views(const fovea::DisplayConfig& config) {
    std::vector<fovea::SceneView> views;
    for (std::size_t i = 0; i < config.channels.size(); ++i) {
        views.push_back(fovea::reference_scene_view(
            fovea::view_from_channel(config.channels[i], config, i), config.reference_scene));
    }
    return views;
}

// The offscreen presenter: one frame, no windows or display, and the colour
// read back from the middle of each output.
int run_offscreen(const Options& options, const fovea::DisplayConfig& config) {
    print_views(config);
    std::vector<fovea::OffscreenOutput> outputs;
    std::vector<fovea::Rgb8> clears;
    for (std::size_t i = 0; i < config.channels.size(); ++i) {
        outputs.push_back({config.channels[i].render_width, config.channels[i].render_height});
        clears.push_back(colour_of(i));
    }
    const auto renderer = fovea::Renderer::offscreen(outputs);
    print_gpu(*renderer);
    if (options.clear) {
        renderer->draw_clear_frame(clears);
    } else {
        renderer->draw_scene_frame(scene_views(config));
    }
    for (std::uint32_t i = 0; i < renderer->output_count(); ++i) {
        const auto px = renderer->read_back_rgba(i);
        const auto& o = outputs[i];
        const std::size_t mid =
            (static_cast<std::size_t>(o.height / 2) * o.width + o.width / 2) * 4;
        std::printf("  offscreen \"%s\": %ux%u, middle rgb(%u,%u,%u)\n",
                    config.channels[i].name.c_str(), o.width, o.height, px[mid], px[mid + 1],
                    px[mid + 2]);
    }
    return finish(*renderer);
}

// One window per channel on its own monitor (the multi_swapchain presenter).
int run_windows(const Options& options, const fovea::DisplayConfig& config) {
    fovea::Platform platform;
    print_monitors(platform);
    const auto problems = fovea::check_monitors(config, platform.monitors());
    if (!problems.empty()) {
        for (const auto& p : problems) {
            (void)std::fprintf(stderr, "%s: %s\n", options.config.c_str(), p.c_str());
        }
        return 1;
    }

    print_views(config);
    std::vector<fovea::Window> windows;
    for (const fovea::ChannelConfig& ch : config.channels) {
        windows.push_back(
            platform.create_window({"Fovea - " + ch.name, static_cast<std::uint32_t>(ch.monitor),
                                    true, 0, 0, !options.identify}));
        const fovea::Window& w = windows.back();
        const auto size = w.pixel_size();
        const int on = w.current_monitor();
        const fovea::Rgb8 c = colour_of(windows.size() - 1);
        if (options.identify || options.clear) {
            std::printf("  window \"%s\": on monitor %d, %ux%u px, %s rgb(%u,%u,%u)\n",
                        ch.name.c_str(), on, size.width, size.height,
                        options.identify ? "identify" : "clear", c.r, c.g, c.b);
        } else {
            std::printf("  window \"%s\": on monitor %d, %ux%u px, scene\n", ch.name.c_str(), on,
                        size.width, size.height);
        }
        // The window manager has the last word on placement; a channel shown
        // on the wrong screen must not go unnoticed.
        if (on != ch.monitor) {
            (void)std::fprintf(stderr,
                               "fovea: channel \"%s\" should be on monitor %d, but its window "
                               "is on monitor %d\n",
                               ch.name.c_str(), ch.monitor, on);
            return 1;
        }
    }

    // Vulkan: one swapchain per window, showing the channel's view of the
    // scene (or, with --clear, its colour). With --identify, the windows are
    // filled in software instead.
    std::unique_ptr<fovea::Renderer> renderer;
    std::vector<fovea::Rgb8> clears;
    const std::vector<fovea::SceneView> views = scene_views(config);
    const auto draw = [&] {
        return options.clear ? renderer->draw_clear_frame(clears)
                             : renderer->draw_scene_frame(views);
    };
    if (!options.identify) {
        std::vector<fovea::WindowOutput> outputs;
        for (std::size_t i = 0; i < windows.size(); ++i) {
            const fovea::ChannelConfig& ch = config.channels[i];
            outputs.push_back({&windows[i], ch.present_mode, ch.render_width, ch.render_height});
            clears.push_back(colour_of(i));
        }
        renderer = fovea::Renderer::for_windows(platform, outputs);
        print_gpu(*renderer);
        (void)draw();
    }
    const auto paint = [&] {
        for (std::size_t i = 0; options.identify && i < windows.size(); ++i) {
            const fovea::Rgb8 c = colour_of(i);
            if (!windows[i].fill(c.r, c.g, c.b)) {
                (void)std::fprintf(stderr, "fovea: cannot fill window %zu\n", i);
            }
        }
    };
    paint();
    if (options.identify) {
        std::printf("Identifying: red, green, blue, yellow, magenta, cyan in config order.\n");
    }
    std::printf("Esc or close a window to quit.\n");
    (void)std::fflush(stdout);

    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                             std::chrono::duration<double>(options.seconds));
    const bool timed = options.seconds > 0.0;
    // The scene on the render thread (step 7): a fixed-rate loop drawing the
    // latest scene state, while this (main) thread handles window events.
    auto scene_state = std::make_unique<fovea::SceneState>();
    std::FILE* stats_file = nullptr;
    std::unique_ptr<fovea::Logger> stats;
    std::unique_ptr<fovea::FrameLoop> loop;
    if (renderer && !options.clear) {
        if (!options.stats.empty()) {
            stats_file = std::fopen(options.stats.c_str(), "w");
            if (stats_file == nullptr) {
                (void)std::fprintf(stderr, "fovea: cannot write %s\n", options.stats.c_str());
                return 1;
            }
            stats = std::make_unique<fovea::Logger>(16384);
            stats->start(fovea::make_text_sink(stats_file));
        }
        loop = std::make_unique<fovea::FrameLoop>(
            *renderer, *scene_state, views, fovea::FrameLoopOptions{config.target_hz}, stats.get());
        loop->start();
    }

    std::uint64_t frames = 0;
    for (;;) {
        if (timed && Clock::now() >= deadline) {
            break;
        }
        if (loop) {
            // Rendering runs on its own thread; wake for events, or at least
            // every 100 ms to check the time limit.
            int timeout_ms = 100;
            if (timed) {
                const auto left =
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
                timeout_ms = static_cast<int>(std::clamp<std::int64_t>(left.count(), 0, 100));
            }
            if (platform.wait_events(timeout_ms).quit) {
                break;
            }
            continue;
        }
        if (renderer) {
            // Draw continuously; FIFO presentation paces the loop at the
            // display rate (the fixed 60 Hz clock arrives in step 7).
            if (platform.poll_events().quit) {
                break;
            }
            if (draw() == 0) {
                // Nothing drawable (every window minimized, or swapchains
                // being rebuilt): nothing paced the frame, so sleep for
                // about one instead of spinning.
                if (platform.wait_events(16).quit) {
                    break;
                }
                continue;
            }
            ++frames;
            continue;
        }
        // Software fill: sleep until an event or the time limit, repainting
        // only when the window system asks.
        int timeout_ms = -1;
        if (timed) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
            timeout_ms = static_cast<int>(std::max<std::int64_t>(left.count(), 0));
        }
        const auto events = platform.wait_events(timeout_ms);
        if (events.quit) {
            break;
        }
        if (events.exposed) {
            paint();
        }
    }
    if (loop) {
        loop->stop();
        if (stats) {
            stats->stop();
            if (stats->dropped() > 0) {
                std::printf("stats: %llu records dropped\n",
                            static_cast<unsigned long long>(stats->dropped()));
            }
            (void)std::fclose(stats_file);
        }
        print_summary(loop->summary());
        return finish(*renderer);
    }
    if (renderer) {
        std::printf("%llu frames\n", static_cast<unsigned long long>(frames));
        return finish(*renderer);
    }
    return 0;
}

int run(const Options& options) {
    if (options.list_monitors) {
        const fovea::Platform platform;
        print_monitors(platform);
        return 0;
    }
    const auto result = fovea::load_display_config(options.config);
    if (!result.config) {
        for (const auto& e : result.errors) {
            (void)std::fprintf(stderr, "%s\n", e.to_string().c_str());
        }
        return 1;
    }
    const fovea::DisplayConfig& config = *result.config;
    switch (config.presenter) {
        case fovea::PresenterKind::multi_swapchain:
            return run_windows(options, config);
        case fovea::PresenterKind::offscreen:
            return run_offscreen(options, config);
        case fovea::PresenterKind::spanned:
            (void)std::fprintf(stderr, "%s: the spanned presenter is not implemented yet\n",
                               options.config.c_str());
            return 1;
    }
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    const auto options = parse_args(argc, argv);
    if (!options) {
        return usage();
    }
    try {
        return run(*options);
    } catch (const std::exception& e) {
        (void)std::fprintf(stderr, "fovea: %s\n", e.what());
        return 1;
    }
}
