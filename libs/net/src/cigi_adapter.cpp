#include "fovea/net/cigi_adapter.hpp"

#include <algorithm>
#include <chrono>

#include "fovea/core/attitude.hpp"
#include "fovea/core/geodesy.hpp"

namespace fovea {

namespace {

// Datagrams handled per poll(): enough for a host at many times the frame
// rate, few enough that a flood can't delay the next Start of Frame much.
constexpr int kMaxMessagesPerPoll = 64;

constexpr double kSecondsPerTick = 10e-6;  // CIGI timestamps (ICD p. 44)

// A message stamped earlier than the last by up to this much arrived out of
// order; further back, the host's clock restarted.
constexpr std::int64_t kReorderWindowTicks = 100000;  // one second

// How fast the host clock's zero may creep later (drift between two
// machines' clocks is tens of ppm).
constexpr double kEpochCreep = 100e-6;

LocalTime::duration ticks_to_duration(std::uint64_t ticks) {
    return std::chrono::duration_cast<LocalTime::duration>(std::chrono::microseconds(ticks * 10));
}

// Removes an entity by moving the last one into its slot (order is not kept).
void remove(SceneSnapshot& scene, EntityState* e) {
    EntityState* last = scene.entities.end() - 1;
    if (e != last) {
        *e = *last;
    }
    scene.entities.pop_back();
}

}  // namespace

CigiAdapter::CigiAdapter(const CigiAdapterOptions& options, FrameStartRing* frame_starts)
    : options_(options),
      frame_starts_(frame_starts),
      socket_(options.port, options.address),
      start_(now()) {}

std::uint32_t CigiAdapter::ig_ticks(LocalTime t) const {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(t - start_).count();
    // Rolls over after about 12 hours, as the ICD expects (p. 216).
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(std::max<std::int64_t>(us, 0)) /
                                      10U);
}

void CigiAdapter::report_frames() {
    if (frame_starts_ == nullptr) {
        return;
    }
    // Only the newest: older ones waited through a stall and are stale.
    std::optional<FrameStart> newest;
    while (const auto f = frame_starts_->try_pop()) {
        if (newest) {
            ++stats_.frames_coalesced;
        }
        newest = f;
    }
    if (!newest) {
        return;
    }
    const std::optional<UdpEndpoint> to = options_.host ? options_.host : last_sender_;
    if (!to) {
        ++stats_.frames_unreported;
        return;
    }
    cigi::StartOfFrame sof;
    sof.ig_mode = mode_;
    sof.timestamp_valid = true;
    sof.timestamp = ig_ticks(newest->t);
    sof.ig_frame_number = static_cast<std::uint32_t>(newest->frame);
    sof.last_host_frame_number = last_host_frame_;
    std::array<std::uint8_t, cigi::kStartOfFrameSize> out{};
    const std::size_t n = cigi::encode(sof, out);
    if (socket_.send_to(std::span(out).first(n), *to)) {
        ++stats_.frames_reported;
    } else {
        ++stats_.frames_unreported;
    }
}

bool CigiAdapter::poll(SceneState& scene) {
    report_frames();
    if (!socket_.wait_readable(options_.wait_ms)) {
        return true;
    }
    bool changed = false;
    for (int i = 0; i < kMaxMessagesPerPoll; ++i) {
        const auto r = socket_.receive(buffer_);
        if (!r) {
            break;
        }
        const LocalTime received = now();
        if (r->truncated) {
            ++stats_.truncated_messages;
            continue;
        }
        if (apply(std::span(buffer_).first(r->size), scene.edit(), received)) {
            last_sender_ = r->from;
            changed = true;
        }
    }
    if (changed) {
        scene.publish();
    }
    return true;
}

bool CigiAdapter::apply(std::span<const std::uint8_t> message, SceneSnapshot& scene,
                        LocalTime received) {
    cigi::MessageReader reader(message, cigi::kIgControlId);
    cigi::MessageReader::Packet packet;
    if (!reader.next(packet)) {
        ++stats_.rejected_messages;
        return false;
    }
    const auto ig = cigi::decode_ig_control(packet.bytes, reader.swapped());
    if (!ig) {
        ++stats_.rejected_messages;
        return false;
    }
    if (ig->timestamp_valid && last_host_ticks_) {
        // Signed: a small step back is a late datagram, a large one a
        // restarted clock; forward steps (rollover included) add up.
        const auto step = static_cast<std::int32_t>(ig->timestamp - *last_host_ticks_);
        if (step < 0 && step >= -kReorderWindowTicks) {
            ++stats_.stale_messages;
            return false;
        }
        if (step < 0) {
            host_epoch_.reset();  // a restarted clock: its zero is placed afresh
        }
        if (step > 0) {
            host_ticks_ += static_cast<std::uint64_t>(step);
        }
    }
    if (ig->timestamp_valid) {
        last_host_ticks_ = ig->timestamp;
        // The host clock's zero as this message places it: no earlier than
        // the true one (the message took some time to arrive), so the
        // earliest seen is the best estimate.
        const LocalTime candidate = received - ticks_to_duration(host_ticks_);
        if (!host_epoch_) {
            host_epoch_ = candidate;
        } else {
            const auto creep = std::chrono::duration_cast<LocalTime::duration>(
                (received - host_epoch_updated_) * kEpochCreep);
            host_epoch_ = std::min(*host_epoch_ + creep, candidate);
        }
        host_epoch_updated_ = received;
    }
    host_time_valid_ = ig->timestamp_valid;
    scene.smoothing_allowed = ig->extrapolation_enable;
    ++stats_.messages;
    last_host_frame_ = ig->host_frame_number;
    if (ig->ig_mode == cigi::IgMode::reset_standby) {
        if (mode_ != cigi::IgMode::reset_standby) {
            scene.entities.clear();    // entering Reset/Standby destroys every entity
            last_host_ticks_.reset();  // a new session may start its clock afresh,
            host_epoch_.reset();       // and its zero is placed afresh
        }
        mode_ = cigi::IgMode::reset_standby;
        ++stats_.ignored_in_standby;
        return true;
    }
    if (mode_ == cigi::IgMode::reset_standby) {
        // The IG leaves Reset/Standby now; the rest of this message was sent
        // while it was there, and is ignored (ICD 2.8, p. 13).
        mode_ = ig->ig_mode;
        ++stats_.ignored_in_standby;
        return true;
    }
    mode_ = ig->ig_mode;

    while (reader.next(packet)) {
        if (packet.id == cigi::kEntityControlId) {
            if (const auto e = cigi::decode_entity_control(packet.bytes, reader.swapped())) {
                apply_entity(*e, scene, received);
            } else {
                ++stats_.rejected_packets;
            }
        } else {
            ++stats_.skipped_packets;
        }
    }
    if (reader.error() != cigi::MessageError::none) {
        ++stats_.rejected_messages;  // packets before the bad one are kept
    }
    return true;
}

void CigiAdapter::apply_entity(const cigi::EntityControl& e, SceneSnapshot& scene,
                               LocalTime received) {
    EntityState* entity = find_entity(scene, e.entity_id);
    if (e.entity_state == cigi::EntityState::destroyed) {
        if (entity != nullptr) {
            remove(scene, entity);
        }
        return;
    }
    if (e.attach) {
        // Child entities arrive in milestone 9. Until then one that was top
        // level is hidden rather than left at its old place.
        ++stats_.attached_entities;
        if (entity != nullptr) {
            entity->active = false;
        }
        return;
    }
    const Geodetic g{e.lat_or_x, e.lon_or_y, e.alt_or_z + options_.geoid_height_m};
    const Euler attitude{static_cast<double>(e.yaw_deg), static_cast<double>(e.pitch_deg),
                         static_cast<double>(e.roll_deg)};
    Pose pose;
    pose.ecef = geodetic_to_ecef(g);
    pose.orientation = body_to_ecef(g, attitude);
    pose.host_t.seconds = static_cast<double>(host_ticks_) * kSecondsPerTick;
    pose.host_t.valid = host_time_valid_;
    pose.recv_t = received;
    pose.sample_t =
        host_time_valid_ && host_epoch_ ? *host_epoch_ + ticks_to_duration(host_ticks_) : received;
    if (entity == nullptr) {
        EntityState created;
        created.id = e.entity_id;
        created.cur = pose;  // no earlier sample: both the same
        created.prev = pose;
        if (!scene.entities.push_back(created)) {
            ++stats_.entity_table_full;
            return;
        }
        entity = scene.entities.end() - 1;
    } else {
        entity->prev = entity->cur;
        entity->cur = pose;
    }
    entity->model = e.entity_type;
    entity->active = e.entity_state == cigi::EntityState::active;
    entity->smooth = e.extrapolation_enable;
    ++stats_.entity_updates;
}

}  // namespace fovea
