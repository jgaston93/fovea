#include "session.hpp"

#include <algorithm>
#include <chrono>
#include <thread>

namespace fovea::test_host {

Session::Session(const UdpEndpoint& ig, std::uint16_t local_port, std::uint32_t local_address,
                 bool smoothing)
    : socket_(local_port, local_address), ig_(ig), start_(now()), smoothing_(smoothing) {}

void Session::read_start_of_frames(LocalTime t) {
    std::array<std::uint8_t, 2048> buffer{};
    while (const auto r = socket_.receive(buffer)) {
        if (r->truncated) {
            continue;
        }
        cigi::MessageReader reader{std::span(buffer).first(r->size), cigi::kStartOfFrameId};
        cigi::MessageReader::Packet p;
        if (!reader.next(p)) {
            continue;
        }
        const auto sof = cigi::decode_start_of_frame(p.bytes, reader.swapped());
        if (!sof) {
            continue;
        }
        ++stats_.start_of_frames;
        last_ig_frame_ = sof->ig_frame_number;
        stats_.last_ig_frame = sof->ig_frame_number;
        operating_ = sof->ig_mode == cigi::IgMode::operate;
        // The host frame it acknowledges, if still remembered.
        auto& sent = sent_[sof->last_host_frame_number % sent_.size()];
        if (operating_ && sent) {
            const double ms = std::chrono::duration<double, std::milli>(t - *sent).count();
            ++stats_.round_trips;
            stats_.round_trip_mean_ms +=
                (ms - stats_.round_trip_mean_ms) / static_cast<double>(stats_.round_trips);
            stats_.round_trip_max_ms = std::max(stats_.round_trip_max_ms, ms);
            sent.reset();  // count each host frame once
        }
    }
}

bool Session::wait_start_of_frame(int timeout_ms) {
    const std::uint64_t before = stats_.start_of_frames;
    if (socket_.wait_readable(timeout_ms)) {
        read_start_of_frames(now());
    }
    return stats_.start_of_frames > before;
}

void Session::wait_until(LocalTime deadline) {
    for (;;) {
        const auto left = deadline - now();
        if (left <= LocalTime::duration::zero()) {
            return;
        }
        // Whole milliseconds for the socket wait, the rest slept exactly.
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
        if (ms == 0) {
            std::this_thread::sleep_until(deadline);
            return;
        }
        if (socket_.wait_readable(static_cast<int>(ms))) {
            read_start_of_frames(now());
        }
    }
}

void Session::send_frame(const FlightState& state, LocalTime t) {
    read_start_of_frames(t);
    ++host_frame_;
    cigi::IgControl ig;
    ig.ig_mode = cigi::IgMode::operate;
    ig.host_frame_number = host_frame_;
    ig.last_ig_frame_number = last_ig_frame_;
    ig.timestamp_valid = true;
    ig.extrapolation_enable = smoothing_;
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t - start_).count();
    ig.timestamp = static_cast<std::uint32_t>(static_cast<std::uint64_t>(us) / 10U);

    std::array<std::uint8_t, cigi::kIgControlSize + cigi::kEntityControlSize> out{};
    std::size_t n = cigi::encode(ig, out);
    // Until the IG reports Operate, IG Control alone (ICD 2.8).
    if (operating_) {
        cigi::EntityControl e;
        e.entity_id = 0;
        e.entity_state = cigi::EntityState::active;
        e.extrapolation_enable = smoothing_;
        e.lat_or_x = state.position.lat_deg;
        e.lon_or_y = state.position.lon_deg;
        e.alt_or_z = state.position.alt_m;
        e.yaw_deg = static_cast<float>(state.attitude.yaw_deg);
        e.pitch_deg = static_cast<float>(state.attitude.pitch_deg);
        e.roll_deg = static_cast<float>(state.attitude.roll_deg);
        n += cigi::encode(e, std::span(out).subspan(n));
    }
    if (socket_.send_to(std::span(out).first(n), ig_)) {
        ++stats_.messages_sent;
        sent_[host_frame_ % sent_.size()] = t;
    }
}

}  // namespace fovea::test_host
