#pragma once

// Displays as the platform layer reports them. Listing them needs SDL
// (step 4); checking a config against a list does not.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "fovea/core/config.hpp"

namespace fovea {

struct MonitorInfo {
    std::string name;
    std::int32_t x = 0;  // desktop position of the top-left corner, pixels
    std::int32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    double refresh_hz = 0.0;
};

// Problems that stop the config's channels being placed on these monitors:
// a monitor index that doesn't exist, or two channels on one monitor (each
// channel gets its own fullscreen window). Empty when the config fits.
std::vector<std::string> check_monitors(const DisplayConfig& config,
                                        std::span<const MonitorInfo> monitors);

}  // namespace fovea
