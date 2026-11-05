#include "fovea/platform/monitors.hpp"

#include <map>

namespace fovea {

std::vector<std::string> check_monitors(const DisplayConfig& config,
                                        std::span<const MonitorInfo> monitors) {
    std::vector<std::string> problems;
    if (config.presenter != PresenterKind::multi_swapchain) {
        return problems;
    }
    std::map<int, std::string> used;
    for (const ChannelConfig& ch : config.channels) {
        if (ch.monitor < 0 || static_cast<std::size_t>(ch.monitor) >= monitors.size()) {
            problems.push_back("channel \"" + ch.name + "\" uses monitor " +
                               std::to_string(ch.monitor) + ", but there are only " +
                               std::to_string(monitors.size()) + " (0 to " +
                               std::to_string(static_cast<long long>(monitors.size()) - 1) + ")");
            continue;
        }
        const auto [it, inserted] = used.emplace(ch.monitor, ch.name);
        if (!inserted) {
            problems.push_back("channels \"" + it->second + "\" and \"" + ch.name +
                               "\" both use monitor " + std::to_string(ch.monitor));
        }
    }
    return problems;
}

}  // namespace fovea
