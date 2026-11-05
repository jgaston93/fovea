// The runtime IG. For now it loads and checks the display config, prints
// the views it would draw, and exits; windows and rendering arrive in
// milestone 1 steps 4 to 7.

#include <cstdio>
#include <string>

#include "fovea/core/config.hpp"
#include "fovea/render/view.hpp"

int main(int argc, char** argv) {
    if (argc != 2) {
        (void)std::fprintf(stderr, "usage: fovea <display-config.toml>\n");
        return 2;
    }
    const auto result = fovea::load_display_config(argv[1]);
    if (!result.config) {
        for (const auto& e : result.errors) {
            (void)std::fprintf(stderr, "%s\n", e.to_string().c_str());
        }
        return 1;
    }
    const fovea::DisplayConfig& config = *result.config;
    std::printf("%zu views at %g Hz:\n", config.channels.size(), config.target_hz);
    for (std::size_t i = 0; i < config.channels.size(); ++i) {
        const auto view = fovea::view_from_channel(config.channels[i], config.near_m,
                                                   static_cast<std::uint32_t>(i));
        const auto& f = view.projection.frustum;
        std::printf("  %-8s monitor %d, %ux%u, yaw %g, frustum [%g, %g] x [%g, %g]\n",
                    view.name.c_str(), config.channels[i].monitor, view.render_width,
                    view.render_height, view.pose.rotation.yaw_deg, f.left_deg, f.right_deg,
                    f.bottom_deg, f.top_deg);
    }
    return 0;
}
