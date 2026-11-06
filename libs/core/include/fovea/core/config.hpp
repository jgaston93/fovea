#pragma once

// Display configuration: the channels and how they are presented
// (architecture.md, "Channel config schema"). Parsed once at startup into
// plain structs; nothing reads TOML afterwards.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fovea/core/attitude.hpp"
#include "fovea/core/math.hpp"

namespace fovea {

enum class PresenterKind : std::uint8_t { multi_swapchain, spanned, offscreen };
enum class PresentMode : std::uint8_t { fifo, mailbox, immediate };

// Half-angles of an asymmetric (off-axis) frustum from the view axis, in
// degrees. Left and bottom are usually negative; each lies in (-90, 90)
// with left < right and bottom < top.
struct Frustum {
    double left_deg = 0.0;
    double right_deg = 0.0;
    double top_deg = 0.0;
    double bottom_deg = 0.0;
};

struct ChannelConfig {
    std::string name;
    int monitor = 0;  // platform monitor index, >= 0; existence is checked at window creation
    PresentMode present_mode = PresentMode::fifo;
    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;
    std::uint32_t entity = 0;  // the scene entity the viewpoint rides on (D-011)
    DVec3 eye_offset_m{0.0};   // from the entity's origin, its body axes
    Euler view_rotation;       // view axis relative to the entity's body (attitude.hpp ranges)
    Frustum frustum;
};

// The milestone 1 reference scene: a flat ground grid below the eye and a
// sky, for checking each channel's view (the [reference_scene] table).
struct ReferenceScene {
    double eye_height_m = 2.0;     // the eye above the ground plane
    double grid_spacing_m = 10.0;  // between grid lines
    double exposure_ev = 0.0;      // tone-mapping exposure, in stops
};

// [host]: where a host program's messages arrive. Without it, nothing
// listens and the views stay where the config puts them.
enum class HostProtocol : std::uint8_t { cigi };

struct HostConfig {
    HostProtocol protocol = HostProtocol::cigi;
    std::uint16_t port = 0;  // UDP port host messages arrive on
    // Where Start of Frame goes: both set, or neither (then the address the
    // host's messages come from). A host that waits for Start of Frame before
    // sending needs them.
    std::optional<std::uint32_t> host_address;  // IPv4, host byte order
    std::uint16_t host_port = 0;
    // The geoid (mean sea level) above the WGS-84 ellipsoid, added to the
    // host's sea-level altitudes; a constant until a geoid model arrives.
    double geoid_height_m = 0.0;
};

struct DisplayConfig {
    PresenterKind presenter = PresenterKind::multi_swapchain;
    double target_hz = 60.0;
    double near_m = 0.1;
    std::vector<ChannelConfig> channels;
    ReferenceScene reference_scene;
    std::optional<HostConfig> host;
};

// One problem in a config file. Line and column are 1-based; 0 when the
// problem has no single location (an unreadable file).
struct ConfigError {
    std::string source;
    std::uint32_t line = 0;
    std::uint32_t column = 0;
    std::string message;

    std::string to_string() const;  // "source:line:column: message"
};

// Every problem found is reported, not just the first; config is set only
// when there are none.
struct ConfigResult {
    std::optional<DisplayConfig> config;
    std::vector<ConfigError> errors;

    bool ok() const { return config.has_value(); }
};

// source names the text in error messages (usually its file path).
ConfigResult parse_display_config(std::string_view text, std::string_view source);
ConfigResult load_display_config(const std::filesystem::path& file);

}  // namespace fovea
