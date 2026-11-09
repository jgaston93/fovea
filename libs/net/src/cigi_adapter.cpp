#include "fovea/net/cigi_adapter.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "fovea/core/attitude.hpp"
#include "fovea/core/celestial.hpp"
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

// Days in a month of the Gregorian calendar.
unsigned days_in_month(std::uint32_t year, std::uint32_t month) {
    constexpr std::array<unsigned, 12> kDays{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29U : kDays.at(month - 1);
}

// Weather Control's standard Layer IDs 0 to 5 (ICD p. 102) are
// WeatherLayer's ids: fog, three clouds, rain, snow.
static_assert(static_cast<std::uint8_t>(cigi::WeatherLayerId::ground_fog) == 0 &&
              static_cast<std::uint8_t>(cigi::WeatherLayerId::cloud_1) == 1 &&
              static_cast<std::uint8_t>(cigi::WeatherLayerId::rain) == 4 &&
              static_cast<std::uint8_t>(cigi::WeatherLayerId::snow) == 5 && kMaxWeatherLayers == 6);

// The Julian date at the start of its minute.
double to_the_minute(double jd) {
    return (std::floor((jd + 0.5) * 1440.0)) / 1440.0 - 0.5;
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
    std::optional<UdpEndpoint> to = options_.host ? options_.host : last_sender_;
    if (to && !options_.host && options_.host_port) {
        to->port = *options_.host_port;
    }
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
    const std::size_t n = cigi::encode(sof, out, cigi::swaps(options_.byte_order));
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
            scene.entities.clear();  // entering Reset/Standby destroys every entity,
            // and puts the environment back as it started (ICD 2.7.3, p. 13)
            scene.environment = options_.environment;
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
        const bool swap = reader.swapped();
        if (packet.id == cigi::kEntityControlId) {
            if (const auto e = cigi::decode_entity_control(packet.bytes, swap)) {
                apply_entity(*e, scene, received);
            } else {
                ++stats_.rejected_packets;
            }
        } else if (packet.id == cigi::kCelestialSphereControlId) {
            if (const auto c = cigi::decode_celestial_sphere_control(packet.bytes, swap)) {
                apply_celestial(*c, scene.environment, received);
            } else {
                ++stats_.rejected_packets;
            }
        } else if (packet.id == cigi::kAtmosphereControlId) {
            if (const auto a = cigi::decode_atmosphere_control(packet.bytes, swap)) {
                apply_atmosphere(*a, scene.environment);
            } else {
                ++stats_.rejected_packets;
            }
        } else if (packet.id == cigi::kWeatherControlId) {
            if (const auto w = cigi::decode_weather_control(packet.bytes, swap)) {
                apply_weather(*w, scene.environment);
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

void CigiAdapter::apply_celestial(const cigi::CelestialSphereControl& c, EnvironmentState& e,
                                  LocalTime received) {
    // The time now, carried on from the last setting, unless the packet
    // sets it.
    double jd = julian_date_at(e, received);
    bool set = false;
    if (c.date_time_valid) {
        if (c.hour > 23 || c.minute > 59) {
            ++stats_.rejected_packets;  // 0-23, 0-59 (p. 83)
            return;
        }
        // MMDDYYYY (p. 84).
        const std::uint32_t month = c.date / 1000000U;
        const std::uint32_t day = c.date / 10000U % 100U;
        const std::uint32_t year = c.date % 10000U;
        if (year < kFirstSkyYear || year > kLastSkyYear || month < 1 || month > 12 || day < 1 ||
            day > days_in_month(year, month)) {
            ++stats_.rejected_packets;
            return;
        }
        const double sent = julian_date(year, month, day, c.hour * 3600.0 + c.minute * 60.0);
        // A host may send its time with every frame: within the minute it
        // names, the clock keeps its seconds, so a running one isn't held
        // to the minute.
        if (std::abs(to_the_minute(jd) - sent) > 0.5 / 86400.0) {
            jd = sent;
            set = true;
        }
    }
    // Rebased only when something changes: every packet adding a rounding
    // to the date would wear it.
    if (set || c.ephemeris_enable != e.time_runs) {
        e.julian_date = jd;
        e.set_at = received;
        e.time_runs = c.ephemeris_enable;
    }
    ++stats_.environment_updates;
}

void CigiAdapter::apply_atmosphere(const cigi::AtmosphereControl& a, EnvironmentState& e) {
    // >= 0 (p. 86), but none at all can't be drawn (nor set in config).
    if (!std::isfinite(a.visibility_range_m) || a.visibility_range_m <= 0.0F) {
        ++stats_.rejected_packets;
        return;
    }
    e.visibility_m = static_cast<double>(a.visibility_range_m);
    ++stats_.environment_updates;
}

void CigiAdapter::apply_weather(const cigi::WeatherControl& w, EnvironmentState& e) {
    const std::optional<WeatherKind> kind = weather_kind_of(w.layer_id);
    if (w.scope != cigi::WeatherScope::global || !kind) {
        ++stats_.weather_ignored;
        return;
    }
    WeatherLayer* found = nullptr;
    for (WeatherLayer& l : e.layers) {
        if (l.id == w.layer_id) {
            found = &l;
        }
    }
    if (!w.weather_enable) {
        if (found != nullptr) {
            *found = *(e.layers.end() - 1);  // order is not kept: the renderer sorts
            e.layers.pop_back();
        }
        ++stats_.environment_updates;
        return;
    }
    if (!std::isfinite(w.base_elevation_m) || !std::isfinite(w.thickness_m) ||
        !std::isfinite(w.visibility_range_m) || !std::isfinite(w.coverage) ||
        w.thickness_m < 0.0F || w.visibility_range_m < 0.0F) {
        ++stats_.rejected_packets;
        return;
    }
    WeatherLayer layer;
    layer.kind = *kind;
    layer.id = w.layer_id;
    layer.base_m = static_cast<double>(w.base_elevation_m) + options_.geoid_height_m;
    layer.thickness_m = static_cast<double>(w.thickness_m);
    layer.coverage = std::clamp(static_cast<double>(w.coverage) / 100.0, 0.0, 1.0);
    layer.visibility_m = static_cast<double>(w.visibility_range_m);
    if (found != nullptr) {
        *found = layer;
    } else {
        // Six ids, six places: never full.
        (void)e.layers.push_back(layer);
    }
    ++stats_.environment_updates;
}

}  // namespace fovea
