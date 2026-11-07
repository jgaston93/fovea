// fovea-compiler: builds a terrain database from fetched sources
// (milestone 3; D-018, D-019). Linux only, as the one user of GDAL.
//
//   fovea-compiler [--jobs N] <settings.toml>
//
// data/compile/khsv.toml builds data/db/khsv from the sources
// tools/fetch-sources.py fetched.

#include <charconv>
#include <cstdio>
#include <exception>
#include <string>
#include <string_view>

#include "compile.hpp"
#include "settings.hpp"

namespace {

int usage() {
    (void)std::fprintf(stderr, "usage: fovea-compiler [--jobs N] <settings.toml>\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    unsigned jobs = 0;
    std::string path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--jobs" && i + 1 < argc) {
            const std::string_view v = argv[++i];
            const auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), jobs);
            if (ec != std::errc{} || end != v.data() + v.size() || jobs == 0 || jobs > 1024) {
                (void)std::fprintf(stderr, "fovea-compiler: --jobs needs 1 to 1024\n");
                return 2;
            }
        } else if (!a.empty() && a[0] != '-' && path.empty()) {
            path = std::string(a);
        } else {
            return usage();
        }
    }
    if (path.empty()) {
        return usage();
    }

    const auto loaded = fovea::compiler::load_settings(path);
    const auto warn = [&] {
        for (const auto& w : loaded.warnings) {
            (void)std::fprintf(stderr, "warning: %s\n", w.c_str());
        }
    };
    if (!loaded.settings) {
        warn();
        for (const auto& e : loaded.errors) {
            (void)std::fprintf(stderr, "%s\n", e.c_str());
        }
        return 1;
    }
    warn();
    const fovea::compiler::CompileSettings& s = *loaded.settings;
    try {
        std::printf("%s: %zu elevation files, levels %d to %d, %u samples a side\n", s.name.c_str(),
                    s.elevation.size(), int{s.root_level}, int{s.finest_level}, s.samples);
        (void)std::fflush(stdout);
        const auto result = fovea::compiler::compile(s, jobs, [](const auto& r) {
            std::printf(
                "level %2d: %6zu tiles in %7.1f s; heights %.1f to %.1f m; "
                "largest error %.2f m\n",
                int{r.level}, r.tiles, r.seconds, static_cast<double>(r.min_m),
                static_cast<double>(r.max_m), static_cast<double>(r.max_error_m));
            (void)std::fflush(stdout);
        });
        if (!result.ok) {
            for (const auto& e : result.errors) {
                (void)std::fprintf(stderr, "%s\n", e.c_str());
            }
            return 1;
        }
        if (result.imagery && result.manifest.imagery) {
            const auto& im = *result.manifest.imagery;
            std::printf("imagery (%s, %u texels a side): %zu tiles in %.1f s, %.2f GB\n",
                        std::string(imagery_format_name(im.format)).c_str(), im.texels,
                        result.imagery->tiles, result.imagery->seconds,
                        static_cast<double>(result.imagery->bytes) / 1e9);
        }
        for (const auto& r : result.runways) {
            std::printf(
                "runway %s: %.0f m; the sources on it %.3f m RMS from the fit, at most %.2f m\n",
                r.name.c_str(), r.length_m, r.rms_m, r.max_abs_m);
        }
        if (const auto& p = result.placements) {
            std::printf("placements: %zu of %zu models in %zu tiles", p->placements, p->models,
                        p->tiles);
            if (!p->largest_rise_model.empty()) {
                std::printf("; the most ground rising under a footprint %.1f m (%s)",
                            p->largest_rise_m, p->largest_rise_model.c_str());
            }
            std::printf("\n");
        }
        std::printf("wrote %s\n", s.output.string().c_str());
    } catch (const std::exception& e) {
        (void)std::fprintf(stderr, "fovea-compiler: %s\n", e.what());
        return 1;
    }
    return 0;
}
