#include "flight.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include <glm/glm.hpp>

namespace fovea::test_host {

namespace {

// Moves `value` toward `target` by at most `max_step`.
double approach(double value, double target, double max_step) {
    return value + std::clamp(target - value, -max_step, max_step);
}

// An angle in (-180, 180].
double wrap180(double deg) {
    deg = std::fmod(deg, 360.0);
    if (deg > 180.0) {
        deg -= 360.0;
    } else if (deg <= -180.0) {
        deg += 360.0;
    }
    return deg;
}

double wrap360(double deg) {
    deg = std::fmod(deg, 360.0);
    return deg < 0.0 ? deg + 360.0 : deg;
}

}  // namespace

Flight::Flight(const FlightState& start, std::vector<Segment> segments, bool loop)
    : state_(start),
      segments_(std::move(segments)),
      loop_(loop),
      roll_deg_(start.attitude.roll_deg) {
    finished_ = segments_.empty();
}

void Flight::advance_segment() {
    in_segment_s = 0.0;
    ++index_;
    if (index_ >= segments_.size()) {
        if (loop_) {
            index_ = 0;
        } else {
            index_ = segments_.size() - 1;
            finished_ = true;
        }
    }
}

void Flight::integrate(const Segment& s, double dt) {
    const double speed = s.kind == Segment::Kind::hold ? 0.0 : s.speed_mps;
    state_.speed_mps = approach(state_.speed_mps, speed, 10.0 * dt);  // 10 m/s^2 at most
    const double v = state_.speed_mps;

    double turn_dps = 0.0;
    double climb = 0.0;
    double target_pitch = 0.0;
    double target_roll = 0.0;
    switch (s.kind) {
        case Segment::Kind::hold:
            break;
        case Segment::Kind::straight:
            climb = std::clamp(s.climb_mps, -v, v);  // never faster than the speed itself
            target_pitch = v > 0.1 ? glm::degrees(std::atan2(climb, v)) : 0.0;
            break;
        case Segment::Kind::turn:
            turn_dps = s.turn_rate_dps;
            break;
        case Segment::Kind::figure_eight:
            // Right for the first full turn, then left.
            turn_dps = in_segment_s < s.duration_s / 2.0 ? std::abs(s.turn_rate_dps)
                                                         : -std::abs(s.turn_rate_dps);
            break;
        case Segment::Kind::roll:
            break;
    }
    if (turn_dps != 0.0) {
        // Coordinated: tan(bank) = v * omega / g.
        target_roll = glm::degrees(std::atan(v * glm::radians(turn_dps) / kGravity));
    }

    const double max_step = kAttitudeRateDps * dt;
    state_.attitude.pitch_deg = approach(state_.attitude.pitch_deg, target_pitch, max_step);
    if (s.kind == Segment::Kind::roll) {
        roll_deg_ += s.roll_rate_dps * dt;
    } else {
        // Back toward the target the short way round after a roll.
        roll_deg_ = approach(wrap180(roll_deg_), target_roll, max_step);
    }
    state_.attitude.roll_deg = wrap180(roll_deg_);
    state_.attitude.yaw_deg = wrap360(state_.attitude.yaw_deg + turn_dps * dt);

    // Along the heading over the ground, scaled by the curvature radii here.
    const double lat = glm::radians(state_.position.lat_deg);
    const double w = 1.0 - wgs84::kE2 * std::sin(lat) * std::sin(lat);
    const double north_radius = wgs84::kA * (1.0 - wgs84::kE2) / (w * std::sqrt(w));
    const double east_radius = wgs84::kA / std::sqrt(w) * std::cos(lat);
    const double ground_speed = std::sqrt(std::max(v * v - climb * climb, 0.0));
    const double yaw = glm::radians(state_.attitude.yaw_deg);
    state_.position.lat_deg += glm::degrees(ground_speed * std::cos(yaw) * dt / north_radius);
    state_.position.lon_deg += glm::degrees(ground_speed * std::sin(yaw) * dt / east_radius);
    state_.position.alt_m += climb * dt;
}

void Flight::step(double dt) {
    // Split at segment ends, so each segment flies exactly its duration and
    // the rest of the step goes to the next.
    while (!finished_ && dt > 0.0) {
        const Segment& s = segments_[index_];
        const double part = std::min(dt, s.duration_s - in_segment_s);
        integrate(s, part);
        in_segment_s += part;
        dt -= part;
        if (in_segment_s >= s.duration_s) {
            advance_segment();
        }
    }
}

}  // namespace fovea::test_host
