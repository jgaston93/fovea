// The runtime IG. Loads the display config, checks it against the connected
// monitors and opens one window per channel, until Esc or a window close.
// Each window shows its channel's view of the reference scene (a ground
// grid, the horizon and the sky).
//
//   fovea [--seconds N] [--stats FILE] [--overlay] [--clear | --identify] <config.toml>
//       run; N > 0: exit after N seconds; --stats: write every frame's timing
//       to FILE as CSV; --overlay: show frame timing on the first channel;
//       --clear: clear each window to its
//       channel's colour (red, green, blue, ... in config order) on the GPU
//       instead; --identify: fill them with those colours in software (no
//       GPU needed)
//   fovea --list-monitors     print the monitors and exit
//
//   fovea --headless [--frames N] [--dump DIR] [--clear] <config.toml>
//       render every channel offscreen (no display needed), N frames, and
//       write each channel's last frame to DIR/<name>.png; an offscreen
//       config does this without --headless

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fovea/core/config.hpp"
#include "fovea/core/image_file.hpp"
#include "fovea/core/log.hpp"
#include "fovea/core/scene.hpp"
#include "fovea/net/cigi_adapter.hpp"
#include "fovea/net/host_thread.hpp"
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
    double seconds = 0.0;      // 0: until Esc
    std::string stats;         // CSV file for per-frame timing, or empty
    bool headless = false;     // render offscreen, whatever the config's presenter
    std::uint32_t frames = 1;  // headless: frames to draw
    std::string dump;          // headless: directory for each channel's PNG
    bool overlay = false;
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
        } else if (args[i] == "--overlay") {
            o.overlay = true;
        } else if (args[i] == "--headless") {
            o.headless = true;
        } else if (args[i] == "--dump" && i + 1 < args.size()) {
            o.dump = std::string(args[++i]);
        } else if (args[i] == "--frames" && i + 1 < args.size()) {
            const std::string_view text = args[++i];
            std::uint32_t n = 0;
            const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
            if (ec != std::errc{} || end != text.data() + text.size() || n == 0 || n > 100000) {
                (void)std::fprintf(stderr, "fovea: --frames needs a count from 1 to 100000\n");
                return std::nullopt;
            }
            o.frames = n;
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
    if ((!o.stats.empty() || o.overlay) && (o.clear || o.identify)) {
        (void)std::fprintf(stderr,
                           "fovea: --stats and --overlay time the scene; they can't be used "
                           "with --clear or --identify\n");
        return std::nullopt;
    }
    return o;
}

int usage() {
    (void)std::fprintf(stderr,
                       "usage: fovea [--seconds N] [--stats FILE] [--overlay] [--clear | "
                       "--identify] <config.toml>\n"
                       "       fovea --headless [--frames N] [--dump DIR] [--clear] <config.toml>\n"
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
    if (s.tiles_resident_max > 0) {
        std::printf(
            "  terrain: up to %u tiles resident, %llu uploaded; %llu frames drew %llu tiles "
            "coarser than wanted (paging behind)\n",
            s.tiles_resident_max, static_cast<unsigned long long>(s.tile_uploads),
            static_cast<unsigned long long>(s.frames_with_misses),
            static_cast<unsigned long long>(s.tile_misses));
    }
    if (s.entity_frames > 0) {
        std::printf(
            "  host samples at frame start: %.2f ms old on the host's clock, %.2f ms "
            "since they arrived (means)\n",
            s.sample_age_mean_ms, s.recv_age_mean_ms);
    }
    if (s.views_below_ground > 0) {
        std::printf(
            "  %llu view frames had the eye at or below the grid (drawn from just "
            "above it; is [reference_scene] origin's altitude right?)\n",
            static_cast<unsigned long long>(s.views_below_ground));
    }
}

int finish(const fovea::Renderer& r) {
    // Paged terrain tiles that couldn't be used (drawn coarser instead).
    const auto problems = r.terrain_problems();
    if (!problems.empty()) {
        std::printf("Terrain: %zu tiles or models couldn't be paged in:\n", problems.size());
        for (std::size_t i = 0; i < std::min<std::size_t>(problems.size(), 10); ++i) {
            std::printf("  %s\n", problems[i].c_str());
        }
        if (problems.size() > 10) {
            std::printf("  ... and %zu more\n", problems.size() - 10);
        }
    }
    std::printf("%llu validation messages\n",
                static_cast<unsigned long long>(r.validation_messages()));
    return r.validation_messages() == 0 ? 0 : 1;
}

// What the CIGI link did, at exit.
void print_host_stats(const fovea::CigiAdapter& adapter) {
    const fovea::CigiAdapterStats& s = adapter.stats();
    std::printf(
        "CIGI: %llu messages (%llu rejected, %llu in Reset/Standby), %llu entity "
        "updates, %llu Start of Frame sent (%llu not)\n",
        static_cast<unsigned long long>(s.messages),
        static_cast<unsigned long long>(s.rejected_messages) +
            static_cast<unsigned long long>(s.truncated_messages),
        static_cast<unsigned long long>(s.ignored_in_standby),
        static_cast<unsigned long long>(s.entity_updates),
        static_cast<unsigned long long>(s.frames_reported),
        static_cast<unsigned long long>(s.frames_unreported));
    if (s.stale_messages > 0 || s.frames_coalesced > 0) {
        std::printf(
            "CIGI: %llu messages out of order (dropped), %llu Start of Frame skipped "
            "after stalls\n",
            static_cast<unsigned long long>(s.stale_messages),
            static_cast<unsigned long long>(s.frames_coalesced));
    }
    if (s.attached_entities > 0 || s.entity_table_full > 0) {
        std::printf(
            "CIGI: %llu attached-entity updates not applied yet, %llu entities over the "
            "table's %zu\n",
            static_cast<unsigned long long>(s.attached_entities),
            static_cast<unsigned long long>(s.entity_table_full), fovea::kMaxEntities);
    }
}

// The config's terrain database, if any (milestone 3 steps 5 and 6): its
// roots loaded now, the rest paged in as views want them. False, after
// saying why, if it can't be used.
bool load_terrain(fovea::Renderer& renderer, const fovea::DisplayConfig& config) {
    if (!config.terrain) {
        return true;
    }
    const fovea::TerrainConfig& t = *config.terrain;
    std::unique_ptr<fovea::TerrainDatabase> db;
    const auto opened = fovea::TerrainDatabase::open(t.database, db);
    if (!db) {
        (void)std::fprintf(stderr, "fovea: cannot open the terrain database %s\n",
                           t.database.string().c_str());
        for (const auto& e : opened.errors) {
            (void)std::fprintf(stderr, "  %s\n", e.c_str());
        }
        return false;
    }
    fovea::TerrainOptions options;
    options.max_level = t.max_level;
    options.pixel_error = t.pixel_error;
    options.max_tiles = t.max_tiles;
    options.budget_bytes = static_cast<std::size_t>(t.budget_mb) << 20;
    const fovea::TerrainLoad load = renderer.load_terrain(std::move(db), options);
    std::printf("Terrain: %s, %zu root tiles (%.2f GB) in %.1f s; the rest paged\n",
                t.database.string().c_str(), load.tiles, static_cast<double>(load.bytes) / 1e9,
                load.seconds);
    if (load.placements > 0) {
        std::printf("  %zu placements of %zu models, loaded as they're paged in\n", load.placements,
                    load.models);
    }
    const std::size_t shown = std::min<std::size_t>(load.problems.size(), 10);
    for (std::size_t i = 0; i < shown; ++i) {
        std::printf("  %s\n", load.problems[i].c_str());
    }
    if (load.problems.size() > shown) {
        std::printf("  ... and %zu more\n", load.problems.size() - shown);
    }
    return true;
}

fovea::RendererOptions renderer_options(const fovea::DisplayConfig& config) {
    fovea::RendererOptions r;
    r.msaa = config.msaa;
    return r;
}

// The date, time and visibility as config sets them (milestone 6, D-029),
// the clock started at `at` when time runs.
fovea::EnvironmentState environment_state(const fovea::EnvironmentConfig& e, fovea::LocalTime at) {
    fovea::EnvironmentState s;
    s.julian_date = fovea::julian_date(e.year, e.month, e.day, e.utc_seconds);
    s.set_at = at;
    s.time_runs = e.time_runs;
    s.visibility_m = e.visibility_m;
    return s;
}

// Headless: every channel rendered offscreen, with no windows or display
// (the offscreen presenter, or --headless with any config). Draws the given
// number of frames, prints the colour read back from the middle of each
// channel, and with --dump writes each channel's last frame as <name>.png.
int run_headless(const Options& options, const fovea::DisplayConfig& config) {
    if (config.host) {
        std::printf(
            "note: [host] is not used headless (recorded host traffic replays in "
            "milestone 2 step 6)\n");
    }
    // Before rendering anything: a bad directory fails at once.
    if (!options.dump.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(options.dump, ec);
        if (ec || !std::filesystem::is_directory(options.dump)) {
            (void)std::fprintf(stderr, "fovea: cannot make the directory %s: %s\n",
                               options.dump.c_str(),
                               ec ? ec.message().c_str() : "a file is in the way");
            return 1;
        }
    }
    print_views(config);
    std::vector<fovea::OffscreenOutput> outputs;
    std::vector<fovea::Rgb8> clears;
    for (std::size_t i = 0; i < config.channels.size(); ++i) {
        outputs.push_back({config.channels[i].render_width, config.channels[i].render_height});
        clears.push_back(colour_of(i));
    }
    const auto renderer = fovea::Renderer::offscreen(outputs, renderer_options(config));
    print_gpu(*renderer);
    if (!options.clear && !load_terrain(*renderer, config)) {
        return 1;
    }
    const auto views = fovea::reference_scene_views(config);
    const fovea::EnvironmentState environment = environment_state(config.environment, fovea::now());
    for (std::uint32_t f = 0; f < options.frames; ++f) {
        renderer->set_sky(fovea::sky_bodies(fovea::julian_date_at(environment, fovea::now())));
        if (options.clear) {
            renderer->draw_clear_frame(clears);
        } else {
            renderer->draw_scene_frame(views);
        }
    }
    for (std::uint32_t i = 0; i < renderer->output_count(); ++i) {
        const auto& o = outputs[i];
        fovea::RgbaImage image{o.width, o.height, renderer->read_back_rgba(i)};
        const std::size_t mid =
            (static_cast<std::size_t>(o.height / 2) * o.width + o.width / 2) * 4;
        std::printf("  offscreen \"%s\": %ux%u, middle rgb(%u,%u,%u)\n",
                    config.channels[i].name.c_str(), o.width, o.height, image.pixels[mid],
                    image.pixels[mid + 1], image.pixels[mid + 2]);
        if (!options.dump.empty()) {
            const auto file =
                std::filesystem::path(options.dump) / (config.channels[i].name + ".png");
            if (!fovea::write_png(file, image)) {
                (void)std::fprintf(stderr, "fovea: cannot write %s\n", file.string().c_str());
                return 1;
            }
            std::printf("  wrote %s\n", file.string().c_str());
        }
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
    const std::vector<fovea::SceneView> views = fovea::reference_scene_views(config);
    // With a host, the views ride its entities (each channel's entity, at its
    // eye offset and rotation), recomputed every frame.
    const std::vector<fovea::ViewDesc> view_descs = fovea::views_from_config(config);
    const fovea::ViewRig rig{view_descs, config.reference_scene,
                             config.host ? config.host->geoid_height_m : 0.0};
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
        renderer = fovea::Renderer::for_windows(platform, outputs, renderer_options(config));
        print_gpu(*renderer);
        if (!options.clear && !load_terrain(*renderer, config)) {
            return 1;
        }
        std::printf("Pacing: %s\n", renderer->has_present_wait()
                                        ? "present wait (each frame starts after a vsync)"
                                        : "FIFO blocking (no present wait support)");
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
    // Config's environment until a host sets it: the host adapter edits the
    // same working copy.
    scene_state->edit().environment = environment_state(config.environment, fovea::now());
    scene_state->publish();
    // Declared before the logger, so the logger stops (flushing) before the
    // file closes, whichever way this function exits.
    std::unique_ptr<std::FILE, int (*)(std::FILE*)> stats_file(nullptr, &std::fclose);
    std::unique_ptr<fovea::Logger> stats;
    // The host link (milestone 2): the network thread receives the host's
    // messages into the scene state and sends a Start of Frame for each of
    // the loop's frames. Declared before the loop, so the loop stops first.
    std::unique_ptr<fovea::FrameStartRing> frame_starts;
    std::unique_ptr<fovea::CigiAdapter> host_adapter;
    std::unique_ptr<fovea::HostThread> host_thread;
    std::unique_ptr<fovea::FrameLoop> loop;
    if (renderer && !options.clear) {
        if (!options.stats.empty()) {
            stats_file.reset(std::fopen(options.stats.c_str(), "w"));
            if (stats_file == nullptr) {
                (void)std::fprintf(stderr, "fovea: cannot write %s\n", options.stats.c_str());
                return 1;
            }
            stats = std::make_unique<fovea::Logger>(std::size_t{16384});
            stats->start(fovea::make_text_sink(stats_file.get()));
        }
        fovea::FrameLoopOptions loop_options{config.target_hz};
        if (options.overlay) {
            renderer->enable_overlay(0);  // the first channel: the debug channel
            loop_options.overlay = true;
        }
        if (config.host) {
            const fovea::HostConfig& h = *config.host;
            fovea::CigiAdapterOptions host_options;
            host_options.port = h.port;
            host_options.address = h.listen_address.value_or(0);
            if (h.host_address) {
                host_options.host = fovea::UdpEndpoint{*h.host_address, h.host_port};
            }
            host_options.geoid_height_m = h.geoid_height_m;
            frame_starts = std::make_unique<fovea::FrameStartRing>();
            try {
                host_adapter =
                    std::make_unique<fovea::CigiAdapter>(host_options, frame_starts.get());
            } catch (const std::runtime_error& e) {
                (void)std::fprintf(stderr, "fovea: CIGI on UDP port %u: %s\n", unsigned{h.port},
                                   e.what());
                return 1;
            }
            std::printf(
                "CIGI 3.3: listening on %s (receive buffer %zu KB); Start of Frame to %s\n",
                fovea::format_endpoint({host_options.address, host_adapter->port()}).c_str(),
                host_adapter->receive_buffer_bytes() / 1024,
                host_options.host ? fovea::format_endpoint(*host_options.host).c_str()
                                  : "the host's address, once it sends");
            loop_options.frame_starts = frame_starts.get();
            loop_options.rig = &rig;
            host_thread = std::make_unique<fovea::HostThread>(*host_adapter, *scene_state);
            host_thread->start();
        }
        loop = std::make_unique<fovea::FrameLoop>(*renderer, *scene_state, views, loop_options,
                                                  stats.get());
        loop->start();
    }

    std::uint64_t frames = 0;
    for (;;) {
        if (timed && Clock::now() >= deadline) {
            break;
        }
        if (loop) {
            // Rendering runs on its own thread; wake for events, or at least
            // every 100 ms to check the time limit and the loop's health.
            if (loop->failed() || (host_thread && host_thread->failed())) {
                break;  // stop() below rethrows what went wrong
            }
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
        // Both threads stop, and both report, before either failure is
        // rethrown (the render thread's first).
        std::exception_ptr render_error;
        try {
            loop->stop();
        } catch (...) {
            render_error = std::current_exception();
        }
        if (host_thread) {
            std::exception_ptr host_error;
            try {
                host_thread->stop();
            } catch (...) {
                host_error = std::current_exception();
            }
            print_host_stats(*host_adapter);
            if (host_error && !render_error) {
                std::rethrow_exception(host_error);
            }
            if (host_error) {
                try {
                    std::rethrow_exception(host_error);
                } catch (const std::exception& e) {
                    (void)std::fprintf(stderr, "fovea: the network thread failed too: %s\n",
                                       e.what());
                }
            }
        }
        if (render_error) {
            std::rethrow_exception(render_error);
        }
        if (stats) {
            stats->stop();
            if (stats->dropped() > 0) {
                std::printf("stats: %llu records dropped\n",
                            static_cast<unsigned long long>(stats->dropped()));
            }
        }
        print_summary(loop->summary());
        if (renderer->overlay_skipped_frames() > 0) {
            std::printf("overlay: left out of %llu frames (a texture upload was pending)\n",
                        static_cast<unsigned long long>(renderer->overlay_skipped_frames()));
        }
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
    if (options.headless || config.presenter == fovea::PresenterKind::offscreen) {
        if (options.overlay || !options.stats.empty() || options.seconds > 0.0 ||
            options.identify) {
            (void)std::fprintf(stderr,
                               "%s: --overlay, --stats, --seconds and --identify are for "
                               "on-screen runs; headless renders --frames frames\n",
                               options.config.c_str());
            return 1;
        }
        return run_headless(options, config);
    }
    if (options.frames != 1 || !options.dump.empty()) {
        (void)std::fprintf(stderr, "fovea: --frames and --dump need --headless\n");
        return 2;
    }
    switch (config.presenter) {
        case fovea::PresenterKind::multi_swapchain:
            return run_windows(options, config);
        case fovea::PresenterKind::offscreen:
            break;  // headless, handled above
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
