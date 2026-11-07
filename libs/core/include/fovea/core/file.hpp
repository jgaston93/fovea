#pragma once

// Whole files into memory. File I/O: never on the render path.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fovea {

// The file's bytes, or nothing with why ("cannot read", "larger than N
// bytes") when it can't be read or is larger than `max_bytes`.
std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& file,
                                                   std::size_t max_bytes, std::string& why);

}  // namespace fovea
