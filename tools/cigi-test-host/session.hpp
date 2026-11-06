#pragma once

// The test host's side of CIGI 3.3 (ICD 2.8, p. 13): ask the IG for Operate
// with IG Control alone, wait for a Start of Frame showing Operate, then send
// IG Control and an Entity Control for entity 0 (the host's main entity)
// each frame. Start of Frame replies are counted, and each one's Last Host
// Frame Number dates the round trip: from sending that host frame to the IG
// starting a frame that had it, timed when the Start of Frame arrives.

#include <array>
#include <cstdint>
#include <optional>

#include "flight.hpp"
#include "fovea/core/time.hpp"
#include "fovea/net/cigi.hpp"
#include "fovea/net/udp.hpp"

namespace fovea::test_host {

struct SessionStats {
    std::uint64_t messages_sent = 0;
    std::uint64_t start_of_frames = 0;
    std::uint32_t last_ig_frame = 0;
    std::uint64_t round_trips = 0;  // Start of Frames whose host frame was found
    double round_trip_mean_ms = 0.0;
    double round_trip_max_ms = 0.0;
};

class Session {
public:
    // Binds `local_port` (0: any free port) on `local_address` (0: all).
    // `smoothing`: allow the IG to extrapolate entity 0 between samples
    // (Extrapolation/Interpolation Enable in IG Control and Entity Control).
    Session(const UdpEndpoint& ig, std::uint16_t local_port, std::uint32_t local_address = 0,
            bool smoothing = true);

    // One host frame: reads any Start of Frames that arrived, then sends IG
    // Control (asking for Operate) and, once the IG reports Operate, entity
    // 0 at `state`.
    void send_frame(const FlightState& state, LocalTime t);

    // Waits up to `timeout_ms` for a Start of Frame and reads it (and any
    // others waiting). True if one arrived. For synchronous mode.
    bool wait_start_of_frame(int timeout_ms);

    // Reads Start of Frames as they arrive until `deadline` (each timed on
    // arrival, so the round trip isn't stretched by the wait for the next
    // send). For asynchronous mode, between sends.
    void wait_until(LocalTime deadline);

    bool operating() const { return operating_; }
    std::uint16_t local_port() const { return socket_.local_port(); }
    const SessionStats& stats() const { return stats_; }

private:
    void read_start_of_frames(LocalTime t);

    UdpSocket socket_;
    UdpEndpoint ig_;
    LocalTime start_;
    bool operating_ = false;
    bool smoothing_ = true;
    std::uint32_t host_frame_ = 0;
    std::uint32_t last_ig_frame_ = 0;
    // When each recent host frame was sent, by frame number modulo the size.
    std::array<std::optional<LocalTime>, 256> sent_{};
    SessionStats stats_;
};

}  // namespace fovea::test_host
