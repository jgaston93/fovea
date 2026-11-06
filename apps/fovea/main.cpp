// The runtime IG. Loads the display config, checks it against the connected
// monitors and opens one window per channel, until Esc or a window close.
// Drawing arrives in milestone 1 steps 5 to 7.
//
//   fovea [--seconds N] [--identify] <display-config.toml>
//       run; N: exit after N seconds; --identify: fill each channel's window
//       with a colour (red, green, blue, ... in config order)
//   fovea --list-monitors     print the monitors and exit

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <string_view>
#include <vector>

#include "fovea/core/config.hpp"
#include "fovea/platform/monitors.hpp"
#include "fovea/platform/platform.hpp"
#include "fovea/render/view.hpp"

namespace {

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

int usage() {
    (void)std::fprintf(stderr,
                       "usage: fovea [--seconds N] [--identify] <display-config.toml>\n"
                       "       fovea --list-monitors\n");
    return 2;
}

int run(int argc, char** argv) {
    std::vector<std::string_view> args(argv + 1, argv + argc);
    if (args.size() == 1 && args[0] == "--list-monitors") {
        const fovea::Platform platform;
        print_monitors(platform);
        return 0;
    }
    double seconds = 0.0;  // 0: until Esc
    bool identify = false;
    while (args.size() > 1 && args[0].starts_with("--")) {
        if (args[0] == "--seconds" && args.size() > 2) {
            seconds = std::strtod(std::string(args[1]).c_str(), nullptr);
            args.erase(args.begin(), args.begin() + 2);
        } else if (args[0] == "--identify") {
            identify = true;
            args.erase(args.begin());
        } else {
            return usage();
        }
    }
    if (args.size() != 1 || args[0].starts_with("--")) {
        return usage();
    }

    const auto result = fovea::load_display_config(std::string(args[0]));
    if (!result.config) {
        for (const auto& e : result.errors) {
            (void)std::fprintf(stderr, "%s\n", e.to_string().c_str());
        }
        return 1;
    }
    const fovea::DisplayConfig& config = *result.config;

    fovea::Platform platform;
    print_monitors(platform);
    const auto problems = fovea::check_monitors(config, platform.monitors());
    if (!problems.empty()) {
        for (const auto& p : problems) {
            (void)std::fprintf(stderr, "%s: %s\n", std::string(args[0]).c_str(), p.c_str());
        }
        return 1;
    }

    std::vector<fovea::Window> windows;
    std::printf("%zu views at %g Hz:\n", config.channels.size(), config.target_hz);
    for (std::size_t i = 0; i < config.channels.size(); ++i) {
        const fovea::ChannelConfig& ch = config.channels[i];
        const auto view = fovea::view_from_channel(ch, config, i);
        windows.push_back(platform.create_window(
            {"Fovea - " + ch.name, static_cast<std::uint32_t>(ch.monitor), true, 0, 0}));
        const fovea::Window& w = windows.back();
        std::printf("  %-8s monitor %d (window on %d, %ux%u px), render %ux%u, yaw %g\n",
                    view.name.c_str(), ch.monitor, w.current_monitor(), w.pixel_width(),
                    w.pixel_height(), view.render_width, view.render_height,
                    view.pose.rotation.yaw_deg);
    }

    // Red, green, blue, then yellow, magenta, cyan for any further channels.
    constexpr std::array<std::array<std::uint8_t, 3>, 6> kColours{{{200, 30, 30},
                                                                   {30, 170, 60},
                                                                   {40, 80, 220},
                                                                   {220, 200, 40},
                                                                   {190, 60, 190},
                                                                   {40, 190, 200}}};
    const auto paint = [&] {
        for (std::size_t i = 0; identify && i < windows.size(); ++i) {
            const auto& c = kColours[i % kColours.size()];
            if (!windows[i].fill(c[0], c[1], c[2])) {
                (void)std::fprintf(stderr, "fovea: cannot fill window %zu\n", i);
            }
        }
    };
    paint();
    if (identify) {
        std::printf("Identifying: red, green, blue, yellow, magenta, cyan in config order.\n");
    }

    std::printf("Esc or close a window to quit.\n");
    (void)std::fflush(stdout);
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (platform.wait_events(50).quit) {
            break;
        }
        paint();  // again, in case the window system discarded the contents
        const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
        if (seconds > 0.0 && elapsed.count() >= seconds) {
            break;
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& e) {
        (void)std::fprintf(stderr, "fovea: %s\n", e.what());
        return 1;
    }
}
