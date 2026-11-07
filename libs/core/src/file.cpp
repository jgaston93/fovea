#include "fovea/core/file.hpp"

#include <fstream>

namespace fovea {

std::optional<std::vector<std::uint8_t>> read_file(const std::filesystem::path& file,
                                                   std::size_t max_bytes, std::string& why) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) {
        why = "cannot read";
        return std::nullopt;
    }
    const std::streamoff size = in.tellg();
    if (size < 0) {
        why = "cannot read";
        return std::nullopt;
    }
    if (static_cast<std::uintmax_t>(size) > max_bytes) {
        why = "larger than " + std::to_string(max_bytes) + " bytes";
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), size)) {
        why = "cannot read";
        return std::nullopt;
    }
    return bytes;
}

}  // namespace fovea
