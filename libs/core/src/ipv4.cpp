#include "fovea/core/ipv4.hpp"

#include <charconv>

namespace fovea {

std::optional<std::uint32_t> parse_ipv4(const std::string& text) {
    std::uint32_t address = 0;
    const char* p = text.data();
    const char* end = text.data() + text.size();
    for (int i = 0; i < 4; ++i) {
        unsigned part = 0;
        const auto [next, ec] = std::from_chars(p, end, part);
        if (ec != std::errc{} || next == p || part > 255 || next - p > 3) {
            return std::nullopt;
        }
        address = (address << 8) | part;
        p = next;
        if (i < 3) {
            if (p == end || *p != '.') {
                return std::nullopt;
            }
            ++p;
        }
    }
    if (p != end) {
        return std::nullopt;
    }
    return address;
}

}  // namespace fovea
