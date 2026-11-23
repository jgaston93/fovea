#pragma once

// An observer's camera (D-057): watching an entity from outside rather than
// riding it, steered by an operator. The state is a bearing, an elevation and
// a distance from the entity in one of three modes (config.hpp's
// ObserverMode); the operator's input moves it at rates, clamped; the camera
// looks at the entity (straight down in top_down).
//
// ObserverControls carries the input across threads: the main thread (which
// reads the keyboard, the mouse and a 3D mouse) writes rates and one-off
// nudges, lock-free; the render thread advances the state once a frame. No
// allocation on either side.

#include <array>
#include <atomic>
#include <cstdint>

#include "fovea/core/config.hpp"
#include "fovea/core/math.hpp"

namespace fovea {

struct ObserverState {
    ObserverMode mode = ObserverMode::orbit;
    double distance_m = 60.0;
    double azimuth_deg = 180.0;  // [0, 360)
    double elevation_deg = 20.0;
};

ObserverState observer_start(const ObserverConfig& config);

// How the operator is moving it: degrees a second round and up, and the
// distance's rate as a fraction a second (positive: away; the distance
// times e^(zoom dt), so a zoom is the same speed near and far).
struct ObserverRates {
    double azimuth_dps = 0.0;
    double elevation_dps = 0.0;
    double zoom_per_s = 0.0;
};

// A 3D mouse's axes as rates (D-058): HID's x right, y toward the user,
// z down, and rx, ry, rz right-handed about them, each -1 to 1. The cap moves
// the entity as if it were held: twisted clockwise, the entity turns
// clockwise (the camera round the other way); its top tilted toward the user,
// more of the entity's top is seen (higher); pulled toward the user, nearer.
// The rest (x, z, ry) do nothing. A dead zone, then a square law for fine
// control near the middle.
inline constexpr double kSpaceMouseDeadZone = 0.08;
inline constexpr double kSpaceMouseAzimuthDps = 90.0;
inline constexpr double kSpaceMouseElevationDps = 60.0;
inline constexpr double kSpaceMouseZoomPerS = 1.5;
ObserverRates observer_rates_6dof(const std::array<double, 6>& axes);

// The state after `dt_s` of these rates plus one-off nudges (degrees, and
// the distance's log): the azimuth wrapped to [0, 360), the elevation and
// distance clamped to config.hpp's limits.
ObserverState observer_step(ObserverState s, const ObserverRates& rates, double dt_s,
                            double nudge_azimuth_deg = 0.0, double nudge_elevation_deg = 0.0,
                            double nudge_log_distance = 0.0);

// The camera for this state watching an entity at `target_ecef` with body
// axes `target_to_ecef`: its position, and its frame (x forward, y right,
// z down) to ECEF. Orbit and tether look at the target, level (no roll);
// top_down looks straight down, north at the top of the view.
struct ObserverEye {
    DVec3 ecef{0.0};
    DQuat to_ecef{1.0, 0.0, 0.0, 0.0};
};
ObserverEye observer_eye(const ObserverState& s, const DVec3& target_ecef,
                         const DQuat& target_to_ecef);

// The operator's input, across threads. Main thread: set_rates (held keys,
// a 3D mouse), nudge (a mouse drag or wheel), request_mode, request_reset.
// Render thread: advance, once a frame.
class ObserverControls {
public:
    explicit ObserverControls(const ObserverConfig& start);

    // Main thread.
    void set_rates(const ObserverRates& rates);
    void nudge(double azimuth_deg, double elevation_deg, double log_distance);
    void request_mode(ObserverMode mode);
    void request_next_mode();  // orbit, tether, top_down, orbit, ...
    void request_reset();      // back to the config's start

    // Render thread: the state after `dt_s` (clamped to 0.1 s, so a stall
    // doesn't fling the camera).
    const ObserverState& advance(double dt_s);
    const ObserverState& state() const { return state_; }

private:
    ObserverConfig start_;
    ObserverState state_;
    std::atomic<double> azimuth_dps_{0.0};
    std::atomic<double> elevation_dps_{0.0};
    std::atomic<double> zoom_per_s_{0.0};
    std::atomic<double> nudge_azimuth_{0.0};
    std::atomic<double> nudge_elevation_{0.0};
    std::atomic<double> nudge_log_distance_{0.0};
    std::atomic<int> mode_request_{-1};
    std::atomic<bool> reset_request_{false};
};

}  // namespace fovea
