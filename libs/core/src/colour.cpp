#include "fovea/core/colour.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace fovea {

namespace {

const std::array<float, 256>& linear_table() {
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (std::size_t i = 0; i < t.size(); ++i) {
            const double c = static_cast<double>(i) / 255.0;
            t[i] =
                static_cast<float>(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
        }
        return t;
    }();
    return table;
}

}  // namespace

float srgb_to_linear(std::uint8_t v) {
    return linear_table()[v];
}

std::uint8_t linear_to_srgb(float v) {
    const double c = std::clamp(static_cast<double>(v), 0.0, 1.0);
    const double s = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
    return static_cast<std::uint8_t>(std::lround(s * 255.0));
}

}  // namespace fovea
