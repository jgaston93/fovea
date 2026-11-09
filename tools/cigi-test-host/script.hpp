#pragma once

// A test host script (TOML): where the IG is, how fast to send, where the
// flight starts, and its segments. tools/cigi-test-host/scripts/ has
// examples.
//
//   [ig]
//   address = "127.0.0.1"   # where the IG listens ([host] port in its config)
//   port = 8004
//   [host]
//   port = 8005             # where Start of Frame comes back (0: any free port,
//                           #   with Fovea's [host] host_port = 0)
//   rate_hz = 60.0          # messages a second (asynchronous mode)
//   byte_order = "network"  # what it sends: "network" (big-endian) or "native"
//   [start]
//   lat = 34.6
//   lon = -86.8
//   alt = 490.0             # metres above sea level
//   heading = 0.0
//   speed = 60.0            # m/s
//   [flight]
//   loop = true
//   [[segment]]
//   kind = "straight"       # hold, straight, turn, roll, figure_eight
//   duration = 10.0         # s (not for figure_eight: two full turns)
//   speed = 60.0            # m/s (straight, turn, roll, figure_eight)
//   climb = 0.0             # m/s (straight)
//   turn_rate = 6.0         # deg/s, positive right (turn, figure_eight)
//   roll_rate = 90.0        # deg/s, positive right (roll)
//
// A segment can also set the environment as it starts (milestone 6 step
// 7), sent once, in that segment's first message:
//
//   date = 2026-03-20       # with time: the date and time, UTC (Celestial
//   time = 11:45:00         # Sphere Control, Date/Time Valid; whole minutes)
//   time_runs = true        # the IG runs the clock on (Ephemeris Model Enable)
//   visibility = 20000.0    # m, global (Atmosphere Control)
//   [[segment.layer]]       # a global weather layer (Weather Control), up to 6
//   id = 0                  # Layer ID: 0 fog, 1 to 3 clouds, 4 rain, 5 snow
//   base = 180.0            # m above sea level
//   thickness = 60.0        # m
//   visibility = 300.0      # m, within it
//   coverage = 100.0        # percent (clouds)
//   enable = true           # false removes the layer

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "flight.hpp"
#include "fovea/net/cigi.hpp"

namespace fovea::test_host {

// What a segment sets as it starts; nothing for most.
struct EnvironmentChange {
    std::optional<cigi::CelestialSphereControl> celestial;
    std::optional<cigi::AtmosphereControl> atmosphere;
    std::vector<cigi::WeatherControl> weather;

    bool empty() const { return !celestial && !atmosphere && weather.empty(); }
};

struct Script {
    std::uint32_t ig_address = 0;  // IPv4, host byte order
    std::uint16_t ig_port = 0;
    std::uint16_t host_port = 8005;  // Fovea's [host] host_port default
    double rate_hz = 60.0;
    cigi::ByteOrder byte_order = cigi::ByteOrder::network;
    FlightState start;
    std::vector<Segment> segments;
    std::vector<EnvironmentChange> environment;  // one for each segment
    bool loop = true;
};

struct ScriptResult {
    std::optional<Script> script;     // set when there are no errors
    std::vector<std::string> errors;  // "file:line: message", in file order
};

ScriptResult parse_script(const std::string& text, const std::string& source);
ScriptResult load_script(const std::filesystem::path& file);

}  // namespace fovea::test_host
