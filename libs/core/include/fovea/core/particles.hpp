#pragma once

// Particle volumes from a host (D-039): spheres of uniform density, each with
// a velocity, in a local east-north-up frame from an origin. A dust, smoke or
// spray simulation sends them; an adapter in net keeps them and publishes
// ParticleFrames, and the renderer draws the latest one. Like SceneState,
// protocol-neutral: nothing here knows how the particles arrived.
//
// A frame is large (a few hundred thousand particles), so it isn't part of
// SceneSnapshot: it has its own triple buffer, its slots sized once at start.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "fovea/core/containers.hpp"
#include "fovea/core/math.hpp"
#include "fovea/core/time.hpp"

namespace fovea {

// A local frame particles are placed in: east-north-up at a point.
struct ParticleOrigin {
    DVec3 ecef{0.0};         // the frame's origin, ECEF
    DMat3 enu_to_ecef{1.0};  // columns: east, north and up, as ECEF unit vectors
};

inline constexpr std::size_t kMaxParticleOrigins = 64;

struct Particle {
    Vec3 position{0.0F};        // [m] east, north, up from its origin
    Vec3 velocity{0.0F};        // [m/s] east, north, up
    float radius = 0.0F;        // [m] the sphere's extent
    float bulk_density = 0.0F;  // [kg/m^3] its mass over the sphere's volume
    LocalTime received;         // when this sample arrived: it moves on its velocity from here
    LocalTime first_seen;       // when the particle first arrived: it fades in from here
    LocalTime fade_from;        // no refresh by then: it fades out from here
    std::uint16_t origin = 0;   // index into ParticleFrame::origins
    bool frozen = false;        // the sender is frozen: not moved on its velocity
};

// The position a particle is drawn at `t`: moved on its velocity from when its
// sample arrived (not before it, and not while frozen), in its origin's frame.
inline Vec3 particle_position_at(const Particle& p, LocalTime t) noexcept {
    if (p.frozen || t <= p.received) {
        return p.position;
    }
    const float dt = std::chrono::duration<float>(t - p.received).count();
    return p.position + p.velocity * dt;
}

// How visible a particle is at `t`, 0 to 1: faded in over `fade` from
// first_seen, out over `fade` from fade_from.
inline float particle_opacity_at(const Particle& p, LocalTime t,
                                 std::chrono::duration<float> fade) noexcept {
    if (fade.count() <= 0.0F) {
        return t < p.fade_from ? 1.0F : 0.0F;
    }
    const float in = std::chrono::duration<float>(t - p.first_seen) / fade;
    const float out = 1.0F - std::chrono::duration<float>(t - p.fade_from) / fade;
    return std::clamp(std::min(in, out), 0.0F, 1.0F);
}

struct ParticleFrame {
    std::vector<Particle> particles;  // sized to the capacity at start; the first `count` are live
    std::size_t count = 0;
    FixedArray<ParticleOrigin, kMaxParticleOrigins> origins;
    std::uint64_t sequence = 0;  // incremented by every publish
};

// One writer thread (the adapter), one reader (the render thread). The
// writer fills write_slot() and publishes; nothing allocates after
// the constructor.
class ParticleState {
public:
    explicit ParticleState(std::size_t capacity) : capacity_(capacity) {
        buffer_.init_slots([capacity](ParticleFrame& f) { f.particles.resize(capacity); });
    }

    std::size_t capacity() const noexcept { return capacity_; }

    // Writer: the slot holds an older frame; overwrite `count` particles and
    // the origins.
    ParticleFrame& write_slot() noexcept { return buffer_.write_slot(); }

    void publish() noexcept {
        buffer_.write_slot().sequence = ++sequence_;
        buffer_.publish();
    }

    // Reader: the newest published frame, valid until the next latest().
    const ParticleFrame& latest() noexcept { return buffer_.read(); }

private:
    std::size_t capacity_;
    std::uint64_t sequence_ = 0;
    TripleBuffer<ParticleFrame> buffer_;
};

}  // namespace fovea
