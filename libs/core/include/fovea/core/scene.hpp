#pragma once

// Protocol-neutral scene state (foundational decision 5): what the renderer
// draws, written by host adapters (CIGI now, others later) and handed to the
// render thread through a triple buffer. Nothing here knows about packets,
// or what the entities are (D-011): a vehicle, a person, a sensor mount.
// Views attach their viewpoint to an entity, or fix it in the world.

#include <cstdint>
#include <utility>

#include "fovea/core/attitude.hpp"
#include "fovea/core/celestial.hpp"
#include "fovea/core/containers.hpp"
#include "fovea/core/math.hpp"
#include "fovea/core/time.hpp"

namespace fovea {

// Time as the host states it, in seconds; its origin is the host's.
struct HostTime {
    double seconds = 0.0;
    bool valid = false;  // the host stamped this sample (CIGI: Timestamp Valid)
};

using EntityId = std::uint32_t;
using ModelId = std::uint32_t;

// One timestamped position and attitude.
struct Pose {
    DVec3 ecef{0.0};
    DQuat orientation{1.0, 0.0, 0.0, 0.0};  // body to ECEF
    HostTime host_t;
    LocalTime recv_t;
    // When the pose held, on this computer's clock: the host's timestamp
    // mapped across (or recv_t without one). Frames are timed on the same
    // clock, so a renderer can extrapolate samples to its frame time.
    LocalTime sample_t;
};

// An articulated part (a rotor, a gear leg), relative to its parent.
struct Articulation {
    std::uint8_t part = 0;
    Euler angles;
};

inline constexpr std::size_t kMaxArticulations = 8;
inline constexpr std::size_t kMaxEntities = 1024;

struct EntityState {
    EntityId id = 0;
    ModelId model = 0;
    // An inactive entity is kept (its model stays loaded) but not drawn, and
    // views can't ride it until it is active again.
    bool active = true;
    // The host allows the renderer to extrapolate or interpolate this
    // entity's samples (CIGI: the entity's Extrapolation/Interpolation
    // Enable); SceneSnapshot::smoothing_allowed must allow it too.
    bool smooth = false;
    // The last two samples, so the renderer can interpolate or extrapolate
    // to its own frame time without the writer knowing about frames.
    Pose cur;
    Pose prev;
    FixedArray<Articulation, kMaxArticulations> parts;
};

// A global weather layer (milestone 6 step 6, D-034): a cloud layer, drawn
// at its base, its coverage shaping procedural noise and shading the ground
// below it; or a band of fog, rain or snow, its visibility the air's within
// it. Heights are above the WGS-84 ellipsoid: writers convert sea-level
// heights with the geoid's (D-020).
enum class WeatherKind : std::uint8_t { cloud, fog, rain, snow };

struct WeatherLayer {
    WeatherKind kind = WeatherKind::cloud;
    double base_m = 0.0;
    double thickness_m = 0.0;
    double coverage = 1.0;         // clouds: the fraction of the sky they cover, 0 to 1
    double visibility_m = 1000.0;  // fog, rain and snow: within the band
};

// Fog, three cloud layers, rain and snow at once: CIGI's global layers less
// its rarer ones (sleet, hail, sand, dust).
inline constexpr std::size_t kMaxWeatherLayers = 6;

// The date and time, and the weather (milestone 6, D-029). The time is
// the UTC Julian date it was at set_at, on this computer's clock: while time
// runs, every frame (and every channel) takes it plus the time since, so a
// writer that doesn't change the time leaves a running clock running.
struct EnvironmentState {
    double julian_date = fovea::julian_date(2026, 3, 20, 15.5 * 3600.0);
    LocalTime set_at;
    // Frozen unless a writer starts it (config's default does), so state
    // made in code, tests' included, draws the same every time.
    bool time_runs = false;
    double visibility_m = 50000.0;                       // drawn from step 3
    FixedArray<WeatherLayer, kMaxWeatherLayers> layers;  // step 6
};

// The UTC Julian date at t.
inline double julian_date_at(const EnvironmentState& e, LocalTime t) noexcept {
    return e.time_runs
               ? e.julian_date + std::chrono::duration<double>(t - e.set_at).count() / 86400.0
               : e.julian_date;
}

struct SceneSnapshot {
    FixedArray<EntityState, kMaxEntities> entities;
    EnvironmentState environment;
    // The host allows smoothing at all (CIGI: IG Control's Extrapolation/
    // Interpolation Enable); off turns it off for every entity.
    bool smoothing_allowed = false;
    std::uint64_t sequence = 0;  // incremented by every publish
};

// The entity with this id, or nullptr. A linear search: fine for the
// tens of entities views attach to, once per view per frame.
inline const EntityState* find_entity(const SceneSnapshot& scene, EntityId id) {
    for (const EntityState& e : scene.entities) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

// The writer's (adapter's) version, on the working snapshot.
inline EntityState* find_entity(SceneSnapshot& scene, EntityId id) {
    return const_cast<EntityState*>(find_entity(std::as_const(scene), id));
}

// One writer thread, one reader thread. Fixed size: publishing and reading
// never allocate. About 1.5 MB, so create it on the heap, once.
class SceneState {
public:
    // Writer side. The working copy keeps its contents between publishes, so
    // an adapter only changes what a packet changed.
    SceneSnapshot& edit() noexcept { return working_; }

    // Hands a copy of the working state to the reader.
    void publish() noexcept {
        ++working_.sequence;
        buffer_.write_slot() = working_;
        buffer_.publish();
    }

    // Reader side: the latest published snapshot. Sequence 0 means nothing
    // has been published yet. The reference is valid only until the next
    // latest() call, which hands the slot back to the writer: call it once
    // per frame and pass the result on (FrameContext::scene).
    const SceneSnapshot& latest() noexcept { return buffer_.read(); }

private:
    SceneSnapshot working_;
    TripleBuffer<SceneSnapshot> buffer_;
};

}  // namespace fovea
