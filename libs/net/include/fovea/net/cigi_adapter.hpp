#pragma once

// The CIGI 3.3 host adapter (milestone 2 step 2): receives host messages over
// UDP, turns them into SceneState, and sends a Start of Frame to the host for
// every rendered frame. It runs on the network thread (HostThread).
//
// Both CIGI modes work without the render thread ever waiting (ICD 2.2): a
// synchronous host answers each Start of Frame at once, an asynchronous host
// sends at its own rate, and every frame draws the newest state that has
// arrived.
//
// From the ICD, as applied here:
// - Each host message starts with IG Control (4.1.1). IG Mode starts at
//   Reset/Standby; in that mode the IG ignores everything else the host
//   sends, including the rest of the message that asks for Operate (ICD 2.8,
//   p. 13: the host waits for a Start of Frame showing Operate before
//   sending other data). Entering Reset/Standby destroys every entity
//   (Start of Frame IG Mode, p. 214), and puts config's environment back
//   (2.7.3, p. 13).
// - Entity Control (4.1.2) for a top-level entity sets its geodetic
//   position (altitude above mean sea level) and yaw, pitch, roll. An entity
//   is created by its first Entity Control and destroyed by Entity State
//   Destroyed. Attached (child) entities come with milestone 9: they're
//   counted, and a top-level entity that becomes attached is made inactive
//   (not drawn, not ridden) until then; Destroyed still removes it.
// - Timestamps are 10 us ticks that roll over after about 12 hours (p. 44);
//   they're unwrapped into host seconds. A message stamped up to a second
//   earlier than the last one arrived out of order and is dropped; a larger
//   step back is a restarted host clock, and counting carries on from it.
// - Start of Frame: after a stall (the network thread held up while frames
//   ran), only the newest frame start is reported, so a synchronous host
//   isn't sent a burst of stale frames.
// - The environment (milestone 6 step 7, D-014 amended), each packet
//   replacing what it carries in the scene's environment, which config set
//   until then:
//   - Celestial Sphere Control (4.1.9) sets the date and time when its
//     Date/Time Valid flag is set, and whether time runs (Ephemeris Model
//     Enable) always; a time within the minute the IG's clock is already
//     in leaves it running, so a host may send its time every frame. Sun,
//     moon and star enables and star intensity aren't
//     taken: the sky draws them all. A date the sky isn't computed for
//     (outside 1950 to 2050), or no date at all, rejects the packet.
//   - Atmosphere Control (4.1.10) sets the global visibility. Humidity,
//     temperature, wind and pressure change nothing drawn yet.
//   - Weather Control (4.1.12) sets or removes (Weather Enable off) a global
//     layer as a whole: Layer IDs 0 to 5 (fog, three clouds, rain, snow),
//     their base (above mean sea level), thickness, visibility and
//     coverage. Sleet, hail, sand, dust and the IG's own layers, regional
//     layers and weather entities are counted and ignored, as is
//     Environmental Region Control (4.1.11).
// - Each sample's time on this computer's clock (Pose::sample_t) maps the
//   host's timestamp across: the host clock's zero is placed where the
//   fastest message so far puts it (least network delay), creeping later at
//   100 ppm so drift between two machines' clocks can't leave it stale. The
//   renderer extrapolates samples to its frame time where the host allows
//   it (Extrapolation/Interpolation Enable, ICD 2.2.1 p. 3-4, 4.1.1, 4.1.2).

#include <array>
#include <cstdint>
#include <optional>

#include "fovea/core/frame_events.hpp"
#include "fovea/core/scene.hpp"
#include "fovea/net/cigi.hpp"
#include "fovea/net/host_adapter.hpp"
#include "fovea/net/udp.hpp"

namespace fovea {

struct CigiAdapterOptions {
    std::uint16_t port = 0;     // where host messages arrive; 0: any free port
    std::uint32_t address = 0;  // the local address to listen on; 0: all interfaces
    // Where Start of Frame goes. Without it, to wherever the last host
    // message came from, so nothing is sent until the host's first message;
    // a host that waits for Start of Frame before sending needs this set.
    std::optional<UdpEndpoint> host;
    // Height of the geoid (mean sea level) above the WGS-84 ellipsoid, added
    // to CIGI altitudes. A constant until a geoid model arrives with terrain
    // (milestone 3); 0 puts sea level on the ellipsoid, which is off by the
    // local geoid height (tens of metres).
    double geoid_height_m = 0.0;
    // The environment as it starts (config's): what Reset/Standby puts back
    // (ICD 2.7.3, p. 13).
    EnvironmentState environment;
    // How long poll() waits for a message: also how late a Start of Frame
    // can leave after its frame starts.
    int wait_ms = 1;
};

// Counts since the adapter started. Read them on the network thread, or
// after it stops.
struct CigiAdapterStats {
    std::uint64_t messages = 0;            // host messages applied
    std::uint64_t rejected_messages = 0;   // malformed, or not starting with a valid IG Control
    std::uint64_t truncated_messages = 0;  // larger than the receive buffer
    std::uint64_t ignored_in_standby = 0;  // messages received in Reset/Standby
    std::uint64_t stale_messages = 0;      // stamped earlier than the last: out of order
    std::uint64_t entity_updates = 0;
    std::uint64_t environment_updates = 0;  // Celestial Sphere, Atmosphere, Weather Control
    std::uint64_t weather_ignored = 0;      // Weather Controls for layers not drawn
    std::uint64_t rejected_packets = 0;     // known packets that didn't decode
    std::uint64_t skipped_packets = 0;      // packets this adapter doesn't handle
    std::uint64_t attached_entities = 0;    // child Entity Controls (milestone 9)
    std::uint64_t entity_table_full = 0;    // new entities beyond kMaxEntities
    std::uint64_t frames_reported = 0;      // Start of Frame messages sent
    std::uint64_t frames_unreported = 0;    // frame starts with no host to send to
    std::uint64_t frames_coalesced = 0;     // older frame starts left out after a stall
};

class CigiAdapter final : public HostAdapter {
public:
    // `frame_starts`: the render thread's frame starts, one Start of Frame
    // each; nullptr sends none. Throws std::runtime_error if the port can't
    // be bound.
    explicit CigiAdapter(const CigiAdapterOptions& options, FrameStartRing* frame_starts = nullptr);

    // Sends a Start of Frame for each frame start, then waits up to wait_ms
    // for host messages and applies them (a bounded number per call, so a
    // flood can't hold up the next Start of Frame), and publishes the scene
    // if anything changed. Always true: the adapter runs until its thread
    // stops.
    bool poll(SceneState& scene) override;

    std::uint16_t port() const { return socket_.local_port(); }
    std::size_t receive_buffer_bytes() const { return socket_.receive_buffer_bytes(); }
    const CigiAdapterStats& stats() const { return stats_; }
    cigi::IgMode ig_mode() const { return mode_; }

private:
    bool apply(std::span<const std::uint8_t> message, SceneSnapshot& scene, LocalTime received);
    void apply_entity(const cigi::EntityControl& e, SceneSnapshot& scene, LocalTime received);
    void apply_celestial(const cigi::CelestialSphereControl& c, EnvironmentState& e,
                         LocalTime received);
    void apply_atmosphere(const cigi::AtmosphereControl& a, EnvironmentState& e);
    void apply_weather(const cigi::WeatherControl& w, EnvironmentState& e);
    void report_frames();
    std::uint32_t ig_ticks(LocalTime t) const;  // 10 us since the adapter started

    CigiAdapterOptions options_;
    FrameStartRing* frame_starts_;
    UdpSocket socket_;
    std::optional<UdpEndpoint> last_sender_;
    LocalTime start_;
    cigi::IgMode mode_ = cigi::IgMode::reset_standby;
    std::uint32_t last_host_frame_ = 0;
    // The host's timestamp, unwrapped: the last raw value and the total.
    std::optional<std::uint32_t> last_host_ticks_;
    std::uint64_t host_ticks_ = 0;
    bool host_time_valid_ = false;  // the current message's Timestamp Valid
    // Where the host clock's zero falls on this computer's clock (see above).
    std::optional<LocalTime> host_epoch_;
    LocalTime host_epoch_updated_;
    CigiAdapterStats stats_;
    // One UDP datagram at most (65,507 bytes of IPv4 payload).
    std::array<std::uint8_t, 65536> buffer_{};
};

}  // namespace fovea
