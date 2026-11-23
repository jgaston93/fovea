#include "fovea/core/observer.hpp"

#include <algorithm>
#include <cmath>

#include "fovea/core/attitude.hpp"
#include "fovea/core/geodesy.hpp"

namespace fovea {

namespace {

double wrap_360(double deg) {
    double w = std::fmod(deg, 360.0);
    if (w < 0.0) {
        w += 360.0;
    }
    return w >= 360.0 ? 0.0 : w;
}

}  // namespace

ObserverState observer_start(const ObserverConfig& config) {
    ObserverState s;
    s.mode = config.mode;
    s.distance_m = std::clamp(config.distance_m, kMinObserverDistanceM, kMaxObserverDistanceM);
    s.azimuth_deg = wrap_360(config.azimuth_deg);
    s.elevation_deg =
        std::clamp(config.elevation_deg, kMinObserverElevationDeg, kMaxObserverElevationDeg);
    return s;
}

namespace {
double shaped(double v) {
    if (!std::isfinite(v)) {
        return 0.0;
    }
    const double m = std::clamp(std::abs(v), 0.0, 1.0);
    if (m <= kSpaceMouseDeadZone) {
        return 0.0;
    }
    const double t = (m - kSpaceMouseDeadZone) / (1.0 - kSpaceMouseDeadZone);
    return std::copysign(t * t, v);
}
}  // namespace

ObserverRates observer_rates_6dof(const std::array<double, 6>& axes) {
    // axes: x, y, z, rx, ry, rz. Twisted clockwise (+rz, about down) turns
    // the entity clockwise: the camera's bearing goes down. Tilted toward
    // the user (+rx, about right: the top toward the user): higher. Pulled
    // toward the user (+y): nearer.
    return {-kSpaceMouseAzimuthDps * shaped(axes[5]), kSpaceMouseElevationDps * shaped(axes[3]),
            -kSpaceMouseZoomPerS * shaped(axes[1])};
}

ObserverState observer_step(ObserverState s, const ObserverRates& rates, double dt_s,
                            double nudge_azimuth_deg, double nudge_elevation_deg,
                            double nudge_log_distance) {
    const double dt = std::isfinite(dt_s) ? std::max(dt_s, 0.0) : 0.0;
    const auto finite = [](double v) { return std::isfinite(v) ? v : 0.0; };
    s.azimuth_deg =
        wrap_360(s.azimuth_deg + finite(rates.azimuth_dps) * dt + finite(nudge_azimuth_deg));
    s.elevation_deg =
        std::clamp(s.elevation_deg + finite(rates.elevation_dps) * dt + finite(nudge_elevation_deg),
                   kMinObserverElevationDeg, kMaxObserverElevationDeg);
    s.distance_m = std::clamp(
        s.distance_m * std::exp(finite(rates.zoom_per_s) * dt + finite(nudge_log_distance)),
        kMinObserverDistanceM, kMaxObserverDistanceM);
    return s;
}

ObserverEye observer_eye(const ObserverState& s, const DVec3& target_ecef,
                         const DQuat& target_to_ecef) {
    const Geodetic g = ecef_to_geodetic(target_ecef);
    const DQuat ned_to_ecef = body_to_ecef(g, Euler{});  // level, facing north: NED itself
    ObserverEye eye;
    if (s.mode == ObserverMode::top_down) {
        eye.ecef = target_ecef + ned_to_ecef * DVec3(0.0, 0.0, -s.distance_m);
        eye.to_ecef = body_to_ecef(g, Euler{0.0, -90.0, 0.0});  // looking down, north up
        return eye;
    }
    double bearing = s.azimuth_deg;
    if (s.mode == ObserverMode::tether) {
        // From the entity's heading: its body's yaw in the local NED.
        bearing += to_euler(glm::conjugate(ned_to_ecef) * target_to_ecef).yaw_deg;
    }
    const double az = glm::radians(bearing);
    const double el = glm::radians(s.elevation_deg);
    const DVec3 offset_ned = s.distance_m * DVec3(std::cos(el) * std::cos(az),
                                                  std::cos(el) * std::sin(az), -std::sin(el));
    eye.ecef = target_ecef + ned_to_ecef * offset_ned;
    // Looking back along the offset: the bearing turned round, pitched down
    // by the elevation, level.
    eye.to_ecef = body_to_ecef(g, Euler{wrap_360(bearing + 180.0), -s.elevation_deg, 0.0});
    return eye;
}

ObserverControls::ObserverControls(const ObserverConfig& start)
    : start_(start), state_(observer_start(start)) {}

void ObserverControls::set_rates(const ObserverRates& rates) {
    azimuth_dps_.store(rates.azimuth_dps, std::memory_order_relaxed);
    elevation_dps_.store(rates.elevation_dps, std::memory_order_relaxed);
    zoom_per_s_.store(rates.zoom_per_s, std::memory_order_relaxed);
}

namespace {
void add(std::atomic<double>& a, double v) {
    double cur = a.load(std::memory_order_relaxed);
    while (!a.compare_exchange_weak(cur, cur + v, std::memory_order_relaxed)) {
    }
}
}  // namespace

void ObserverControls::nudge(double azimuth_deg, double elevation_deg, double log_distance) {
    add(nudge_azimuth_, azimuth_deg);
    add(nudge_elevation_, elevation_deg);
    add(nudge_log_distance_, log_distance);
}

void ObserverControls::request_mode(ObserverMode mode) {
    mode_request_.store(static_cast<int>(mode), std::memory_order_relaxed);
}

namespace {
constexpr int kNextMode = -2;
constexpr int kNoMode = -1;
}  // namespace

void ObserverControls::request_next_mode() {
    mode_request_.store(kNextMode, std::memory_order_relaxed);
}

void ObserverControls::request_reset() {
    reset_request_.store(true, std::memory_order_relaxed);
}

const ObserverState& ObserverControls::advance(double dt_s) {
    // A reset takes all: the config's start, whatever else came with it.
    const bool reset = reset_request_.exchange(false, std::memory_order_relaxed);
    const int mode = mode_request_.exchange(kNoMode, std::memory_order_relaxed);
    if (reset) {
        state_ = observer_start(start_);
        set_rates({});
        nudge_azimuth_.store(0.0, std::memory_order_relaxed);
        nudge_elevation_.store(0.0, std::memory_order_relaxed);
        nudge_log_distance_.store(0.0, std::memory_order_relaxed);
        return state_;
    }
    if (mode >= 0 && mode <= static_cast<int>(ObserverMode::top_down)) {
        state_.mode = static_cast<ObserverMode>(mode);
    } else if (mode == kNextMode) {
        state_.mode = static_cast<ObserverMode>((static_cast<int>(state_.mode) + 1) %
                                                (static_cast<int>(ObserverMode::top_down) + 1));
    }
    const ObserverRates rates{azimuth_dps_.load(std::memory_order_relaxed),
                              elevation_dps_.load(std::memory_order_relaxed),
                              zoom_per_s_.load(std::memory_order_relaxed)};
    state_ = observer_step(state_, rates, std::min(dt_s, 0.1),
                           nudge_azimuth_.exchange(0.0, std::memory_order_relaxed),
                           nudge_elevation_.exchange(0.0, std::memory_order_relaxed),
                           nudge_log_distance_.exchange(0.0, std::memory_order_relaxed));
    return state_;
}

}  // namespace fovea
