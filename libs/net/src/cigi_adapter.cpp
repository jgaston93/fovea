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

EntityState* find(SceneSnapshot& scene, EntityId id) {
    for (EntityState& e : scene.entities) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
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
    : options_(options), frame_starts_(frame_starts), socket_(options.port), start_(now()) {}

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
    while (const auto f = frame_starts_->try_pop()) {
        const std::optional<UdpEndpoint> to = options_.host ? options_.host : last_sender_;
        if (!to) {
            ++stats_.frames_unreported;
            continue;
        }
        cigi::StartOfFrame sof;
        sof.ig_mode = mode_;
        sof.timestamp_valid = true;
        sof.timestamp = ig_ticks(f->t);
        sof.ig_frame_number = static_cast<std::uint32_t>(f->frame);
        sof.last_host_frame_number = last_host_frame_;
        std::array<std::uint8_t, cigi::kStartOfFrameSize> out{};
        const std::size_t n = cigi::encode(sof, out);
        if (socket_.send_to(std::span(out).first(n), *to)) {
            ++stats_.frames_reported;
        } else {
            ++stats_.frames_unreported;
        }
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
    ++stats_.messages;
    last_host_frame_ = ig->host_frame_number;
    if (ig->timestamp_valid) {
        // Unsigned difference: correct across the 32-bit rollover.
        host_ticks_ += last_host_ticks_ ? ig->timestamp - *last_host_ticks_ : 0U;
        last_host_ticks_ = ig->timestamp;
    }
    if (ig->ig_mode == cigi::IgMode::reset_standby) {
        if (mode_ != cigi::IgMode::reset_standby) {
            scene.entities.clear();  // entering Reset/Standby destroys every entity
        }
        mode_ = cigi::IgMode::reset_standby;
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
    if (e.attach) {
        ++stats_.attached_entities;
        return;
    }
    EntityState* entity = find(scene, e.entity_id);
    if (e.entity_state == cigi::EntityState::destroyed) {
        if (entity != nullptr) {
            remove(scene, entity);
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
    pose.recv_t = received;
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
    ++stats_.entity_updates;
}

}  // namespace fovea
