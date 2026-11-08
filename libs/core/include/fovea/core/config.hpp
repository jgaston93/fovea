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
#include "fovea/core/geodesy.hpp"
#include "fovea/core/containers.hpp"
#include "fovea/core/math.hpp"
#include "fovea/core/scene.hpp"

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
    double eye_height_m = 2.0;     // the eye above the ground plane, with no host
    double grid_spacing_m = 10.0;  // between grid lines
    double exposure_ev = 0.0;      // tone-mapping exposure, in stops
    // The ground grid is the level plane through this point (milestone 2):
    // latitude and longitude in degrees, altitude in metres above sea level
    // (like a host's; [host] geoid_height_m applies to both).
    Geodetic origin;
};

// [terrain]: a terrain database to draw (milestone 3): its roots loaded up
// front, the rest paged in as views need them, down to max_level. Without
// it, the reference scene's grid only.
struct TerrainConfig {
    // The database directory; a relative path is relative to the config
    // file (load_display_config resolves it; parse_display_config leaves it).
    std::filesystem::path database;
    int max_level = 30;              // tiles finer than this aren't loaded
    double pixel_error = 1.0;        // a tile splits where its error would show as more (D-016)
    std::uint32_t max_tiles = 4096;  // on the GPU at once (paging, step 6)
    std::uint32_t budget_mb = 2048;  // GPU memory the tiles may take
};

// [host]: where a host program's messages arrive. Without it, nothing
// listens and the views stay where the config puts them.
enum class HostProtocol : std::uint8_t { cigi };

struct HostConfig {
    HostProtocol protocol = HostProtocol::cigi;
    std::uint16_t port = 0;  // UDP port host messages arrive on
    // The local address to listen on: all interfaces when unset. 127.0.0.1
    // takes messages from this computer only (a host running beside Fovea),
    // which also keeps Windows Firewall from asking.
    std::optional<std::uint32_t> listen_address;
    // Where Start of Frame goes: both set, or neither (then the address the
    // host's messages come from). A host that waits for Start of Frame before
    // sending needs them.
    std::optional<std::uint32_t> host_address;  // IPv4, host byte order
    std::uint16_t host_port = 0;
    // The geoid (mean sea level) above the WGS-84 ellipsoid, added to the
    // host's sea-level altitudes; a constant until a geoid model arrives.
    double geoid_height_m = 0.0;
};

// [environment] (milestone 6, D-029): the date and time, UTC, and the
// visibility, until a host sets them.
struct EnvironmentConfig {
    std::int64_t year = 2026;
    unsigned month = 3;
    unsigned day = 20;
    double utc_seconds = 15.5 * 3600.0;  // since midnight
    bool time_runs = true;
    double visibility_m = 50000.0;
    // [[environment.layer]] (step 6): heights above mean sea level, as a
    // host gives them, converted with [host]'s geoid height.
    FixedArray<WeatherLayer, kMaxWeatherLayers> layers;
};

struct DisplayConfig {
    PresenterKind presenter = PresenterKind::multi_swapchain;
    double target_hz = 60.0;
    double near_m = 0.1;
    // Samples per pixel of each view's scene (milestone 5, D-027): 1, 2, 4
    // or 8; the renderer refuses a count the GPU can't do.
    std::uint32_t msaa = 4;
    std::vector<ChannelConfig> channels;
    ReferenceScene reference_scene;
    EnvironmentConfig environment;
    std::optional<HostConfig> host;
    std::optional<TerrainConfig> terrain;
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
