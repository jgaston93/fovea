#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace fovea {

// "a.b.c.d" (each part 0-255, at most three digits) to an IPv4 address in
// host byte order (127.0.0.1 is 0x7F000001), or nothing.
std::optional<std::uint32_t> parse_ipv4(const std::string& text);

}  // namespace fovea
