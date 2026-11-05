#pragma once

// Protocol-neutral scene state (foundational decision 5): what the renderer
// draws, written by host adapters (CIGI now, DIS later) and handed to the
// render thread through a triple buffer. Nothing here knows about packets.

#include <cstdint>

#include "fovea/core/attitude.hpp"
#include "fovea/core/containers.hpp"
#include "fovea/core/math.hpp"
#include "fovea/core/time.hpp"

namespace fovea {

// Time as the host states it, in seconds; its origin is the host's.
struct HostTime {
    double seconds = 0.0;
};

using EntityId = std::uint32_t;
using ModelId = std::uint32_t;

// One timestamped position and attitude.
struct Pose {
    DVec3 ecef{0.0};
    DQuat orientation{1.0, 0.0, 0.0, 0.0};  // body to ECEF
    HostTime host_t;
    LocalTime recv_t;
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
    // The last two samples, so the renderer can interpolate or extrapolate
    // to its own frame time without the writer knowing about frames.
    Pose cur;
    Pose prev;
    FixedArray<Articulation, kMaxArticulations> parts;
};

struct EnvironmentState {
    double time_of_day_h = 12.0;  // local solar time, hours
};

struct SceneSnapshot {
    Pose eyepoint;  // the ownship's design eye point
    FixedArray<EntityState, kMaxEntities> entities;
    EnvironmentState environment;
    std::uint64_t sequence = 0;  // incremented by every publish
};

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
    // has been published yet.
    const SceneSnapshot& latest() noexcept { return buffer_.read(); }

private:
    SceneSnapshot working_;
    TripleBuffer<SceneSnapshot> buffer_;
};

}  // namespace fovea
