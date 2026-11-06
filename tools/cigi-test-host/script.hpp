#pragma once

// A test host script (TOML): where the IG is, how fast to send, where the
// flight starts, and its segments. tools/cigi-test-host/scripts/ has
// examples.
//
//   [ig]
//   address = "127.0.0.1"   # where the IG listens ([host] port in its config)
//   port = 8004
//   [host]
//   port = 8005             # where Start of Frame comes back (0: any free port)
//   rate_hz = 60.0          # messages a second (asynchronous mode)
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

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "flight.hpp"

namespace fovea::test_host {

struct Script {
    std::uint32_t ig_address = 0;  // IPv4, host byte order
    std::uint16_t ig_port = 0;
    std::uint16_t host_port = 0;
    double rate_hz = 60.0;
    FlightState start;
    std::vector<Segment> segments;
    bool loop = true;
};

struct ScriptResult {
    std::optional<Script> script;     // set when there are no errors
    std::vector<std::string> errors;  // "file:line: message", in file order
};

ScriptResult parse_script(const std::string& text, const std::string& source);
ScriptResult load_script(const std::filesystem::path& file);

}  // namespace fovea::test_host
