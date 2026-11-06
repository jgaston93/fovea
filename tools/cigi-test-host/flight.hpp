#pragma once

// The CIGI test host's motion (milestone 2 step 4): a scripted flight of
// simple kinematic segments, stepped at the host's send rate. Not a flight
// model, just smooth, predictable motion to look at and measure: speed along
// the heading, climb, coordinated turns (bank from turn rate and speed), and
// rolls. Bank and pitch ease toward each segment's targets, so segments join
// without jumps.

#include <string>
#include <vector>

#include "fovea/core/attitude.hpp"
#include "fovea/core/geodesy.hpp"

namespace fovea::test_host {

struct Segment {
    enum class Kind { hold, straight, turn, roll, figure_eight };
    Kind kind = Kind::straight;
    double duration_s = 0.0;  // figure_eight: two full turns, so 720 / |turn_rate|
    double speed_mps = 0.0;
    double climb_mps = 0.0;      // straight
    double turn_rate_dps = 0.0;  // turn, figure_eight: positive turns right
    double roll_rate_dps = 0.0;  // roll: positive rolls right
};

struct FlightState {
    Geodetic position;  // altitude above sea level (as CIGI sends it)
    Euler attitude;     // yaw from true north, pitch, roll
    double speed_mps = 0.0;
};

class Flight {
public:
    // `loop`: start again from the first segment after the last; otherwise
    // hold the last state.
    Flight(const FlightState& start, std::vector<Segment> segments, bool loop);

    // Advances by dt seconds.
    void step(double dt);

    const FlightState& state() const { return state_; }
    bool finished() const { return finished_; }
    std::size_t segment_index() const { return index_; }

private:
    void advance_segment();

    FlightState state_;
    std::vector<Segment> segments_;
    bool loop_ = false;
    std::size_t index_ = 0;
    double in_segment_s = 0.0;
    double roll_deg_ = 0.0;  // continuous for roll segments, then wrapped
    bool finished_ = false;
};

// Bank and pitch change at most this fast between targets.
inline constexpr double kAttitudeRateDps = 30.0;
inline constexpr double kGravity = 9.80665;

}  // namespace fovea::test_host
