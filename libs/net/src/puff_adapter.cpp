#include "fovea/net/puff_adapter.hpp"

#include <bit>
#include <cmath>
#include <numbers>

#include "fovea/core/geodesy.hpp"

namespace fovea {

namespace {

constexpr int kMaxDatagramsPerPoll = 4096;
constexpr auto kLossWindow = std::chrono::seconds(1);
constexpr auto kForgetOrigin = std::chrono::seconds(10);

std::size_t hash_of(std::uint32_t source, std::uint32_t id) noexcept {
    const std::uint64_t k = (std::uint64_t{source} << 32) | id;
    return static_cast<std::size_t>((k * 0x9E3779B97F4A7C15ULL) >> 17);
}

bool finite_puff(const puff::Puff& p) noexcept {
    return std::isfinite(p.east) && std::isfinite(p.north) && std::isfinite(p.up) &&
           std::isfinite(p.v_east) && std::isfinite(p.v_north) && std::isfinite(p.v_up) &&
           std::isfinite(p.radius) && std::isfinite(p.bulk_density) && p.radius >= 0.0F &&
           p.bulk_density >= 0.0F;
}

}  // namespace

PuffAdapter::PuffAdapter(const PuffAdapterOptions& options, ParticleState& state)
    : options_(options), state_(state), socket_(options.port, options.address) {
    held_.resize(state.capacity());
    index_.resize(std::bit_ceil(std::max<std::size_t>(16, state.capacity() * 2)));
}

bool PuffAdapter::poll() {
    if (socket_.wait_readable(options_.wait_ms)) {
        for (int i = 0; i < kMaxDatagramsPerPoll; ++i) {
            const auto r = socket_.receive(buffer_);
            if (!r) {
                break;
            }
            if (r->truncated) {
                stats_.datagrams.fetch_add(1, std::memory_order_relaxed);
                stats_.rejected.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            apply(std::span<const std::uint8_t>(buffer_).first(r->size), now());
        }
    }
    const LocalTime t = now();
    if (t - last_publish_ >= options_.publish_period) {
        publish(t);
    }
    return true;
}

void PuffAdapter::apply(std::span<const std::uint8_t> d, LocalTime t) noexcept {
    stats_.datagrams.fetch_add(1, std::memory_order_relaxed);
    puff::Header h;
    switch (puff::read_header(d, h)) {
        case puff::HeaderResult::rejected:
            stats_.rejected.fetch_add(1, std::memory_order_relaxed);
            return;
        case puff::HeaderResult::unknown_type:
            stats_.skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        case puff::HeaderResult::ok:
            break;
    }
    Source* s = source_for(h);
    if (s == nullptr) {
        stats_.rejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    count_sequence(*s, h.sequence);
    const auto source = static_cast<std::uint32_t>(s - sources_.begin());
    if (h.type == static_cast<std::uint8_t>(puff::Type::origin)) {
        apply_origin(source, h, puff::read_origin(d, h), t);
    } else {
        apply_puffs(source, d, h, t);
    }
}

PuffAdapter::Source* PuffAdapter::source_for(const puff::Header& h) noexcept {
    for (std::size_t i = 0; i < sources_.size(); ++i) {
        Source& s = sources_[i];
        if (s.id != h.source_id) {
            continue;
        }
        if (s.session != h.session) {
            // Restarted: its time starts again, so its old puffs and origins
            // would win against everything new.
            reset_source(static_cast<std::uint32_t>(i));
            s.session = h.session;
            stats_.sessions.fetch_add(1, std::memory_order_relaxed);
        }
        return &s;
    }
    Source fresh;
    fresh.id = h.source_id;
    fresh.session = h.session;
    if (!sources_.push_back(fresh)) {
        return nullptr;
    }
    stats_.sessions.fetch_add(1, std::memory_order_relaxed);
    return &sources_[sources_.size() - 1];
}

void PuffAdapter::reset_source(std::uint32_t index) noexcept {
    std::size_t kept = 0;
    for (std::size_t i = 0; i < count_; ++i) {
        if (held_[i].source != index) {
            held_[kept++] = held_[i];
        }
    }
    count_ = kept;
    rebuild_index();
    for (OriginSlot& o : origins_) {
        if (o.used && o.source == index) {
            o.used = false;
        }
    }
    sources_[index].window_open = false;
}

void PuffAdapter::count_sequence(Source& s, std::uint32_t sequence) noexcept {
    if (!s.window_open) {
        s.window_open = true;
        s.base = sequence;
        s.lowest = s.highest = 0;
        s.received = 1;
        return;
    }
    const auto d = static_cast<std::int64_t>(static_cast<std::int32_t>(sequence - s.base));
    s.lowest = std::min(s.lowest, d);
    s.highest = std::max(s.highest, d);
    ++s.received;
}

int PuffAdapter::find_origin(std::uint32_t source, std::uint32_t id) const noexcept {
    for (std::size_t i = 0; i < origins_.size(); ++i) {
        if (origins_[i].used && origins_[i].source == source && origins_[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void PuffAdapter::apply_origin(std::uint32_t source, const puff::Header& h, const puff::Origin& o,
                               LocalTime t) noexcept {
    int slot = find_origin(source, h.origin_id);
    if (slot < 0) {
        for (std::size_t i = 0; i < origins_.size() && slot < 0; ++i) {
            if (!origins_[i].used) {
                slot = static_cast<int>(i);
            }
        }
    }
    if (slot < 0 || !std::isfinite(o.latitude_rad) || !std::isfinite(o.longitude_rad) ||
        !std::isfinite(o.height_msl_m)) {
        (slot < 0 ? stats_.origins_full : stats_.rejected).fetch_add(1, std::memory_order_relaxed);
        return;
    }
    constexpr double kDeg = 180.0 / std::numbers::pi;
    const double lat = o.latitude_rad * kDeg;
    const double lon = o.longitude_rad * kDeg;
    OriginSlot& s = origins_[static_cast<std::size_t>(slot)];
    s.used = true;
    s.source = source;
    s.id = h.origin_id;
    s.last_used = t;
    s.frame.ecef = geodetic_to_ecef({lat, lon, o.height_msl_m + options_.geoid_height_m});
    const DMat3 ned = ned_to_ecef(lat, lon);  // columns: north, east, down
    s.frame.enu_to_ecef = DMat3(ned[1], ned[0], -ned[2]);
    stats_.origins.fetch_add(1, std::memory_order_relaxed);
}

void PuffAdapter::apply_puffs(std::uint32_t source, std::span<const std::uint8_t> d,
                              const puff::Header& h, LocalTime t) noexcept {
    const int slot = find_origin(source, h.origin_id);
    if (slot < 0) {
        stats_.unknown_origin.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    origins_[static_cast<std::size_t>(slot)].last_used = t;
    const LocalTime fade_from =
        t + std::chrono::duration_cast<Clock::duration>(std::chrono::microseconds(h.hold_us));
    const bool frozen = (h.flags & puff::kFlagFrozen) != 0;
    for (std::size_t i = 0; i < h.count; ++i) {
        const puff::Puff r = puff::read_puff(d, h, i);
        if (!finite_puff(r)) {
            stats_.rejected.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        std::size_t at = find(source, r.id);
        if (at != npos) {
            if (held_[at].sim_time_ns > h.sim_time_ns) {
                stats_.late.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        } else {
            if (count_ == held_.size()) {
                stats_.table_full.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            at = count_++;
            held_[at].source = source;
            held_[at].id = r.id;
            held_[at].p.first_seen = t;
            index_insert(at);
        }
        Held& e = held_[at];
        e.sim_time_ns = h.sim_time_ns;
        e.p.position = Vec3(r.east, r.north, r.up);
        e.p.velocity = Vec3(r.v_east, r.v_north, r.v_up);
        e.p.radius = r.radius;
        e.p.bulk_density = r.bulk_density;
        e.p.received = t;
        e.p.fade_from = fade_from;
        e.p.origin = static_cast<std::uint16_t>(slot);
        e.p.frozen = frozen;
        stats_.records.fetch_add(1, std::memory_order_relaxed);
    }
}

std::size_t PuffAdapter::find(std::uint32_t source, std::uint32_t id) const noexcept {
    const std::size_t mask = index_.size() - 1;
    for (std::size_t i = hash_of(source, id) & mask;; i = (i + 1) & mask) {
        const std::uint32_t v = index_[i];
        if (v == 0) {
            return npos;
        }
        const Held& e = held_[v - 1];
        if (e.source == source && e.id == id) {
            return v - 1;
        }
    }
}

void PuffAdapter::index_insert(std::size_t at) noexcept {
    // At most half full (the index is twice the capacity), so a slot is found.
    const std::size_t mask = index_.size() - 1;
    std::size_t i = hash_of(held_[at].source, held_[at].id) & mask;
    while (index_[i] != 0) {
        i = (i + 1) & mask;
    }
    index_[i] = static_cast<std::uint32_t>(at + 1);
}

void PuffAdapter::rebuild_index() noexcept {
    std::fill(index_.begin(), index_.end(), 0U);
    for (std::size_t i = 0; i < count_; ++i) {
        index_insert(i);
    }
}

void PuffAdapter::compact(LocalTime t) noexcept {
    const auto fade = std::chrono::duration_cast<Clock::duration>(options_.fade);
    std::array<bool, kMaxParticleOrigins> named{};
    std::size_t kept = 0;
    for (std::size_t i = 0; i < count_; ++i) {
        if (t < held_[i].p.fade_from + fade) {
            named.at(held_[i].p.origin) = true;
            if (kept != i) {
                held_[kept] = held_[i];
            }
            ++kept;
        }
    }
    if (kept != count_) {
        count_ = kept;
        rebuild_index();
    }
    for (std::size_t i = 0; i < origins_.size(); ++i) {
        OriginSlot& o = origins_.at(i);
        if (o.used && !named.at(i) && t - o.last_used > kForgetOrigin) {
            o.used = false;
        }
    }
}

void PuffAdapter::publish(LocalTime t) noexcept {
    if (t - window_start_ >= kLossWindow) {
        for (Source& s : sources_) {
            if (s.window_open) {
                const auto expected = static_cast<std::uint64_t>(s.highest - s.lowest + 1);
                if (expected > s.received) {
                    stats_.lost.fetch_add(expected - s.received, std::memory_order_relaxed);
                }
                s.window_open = false;
            }
        }
        window_start_ = t;
    }
    compact(t);
    ParticleFrame& f = state_.write_slot();
    for (std::size_t i = 0; i < count_; ++i) {
        f.particles[i] = held_[i].p;
    }
    f.count = count_;
    f.origins.clear();
    for (const OriginSlot& o : origins_) {
        (void)f.origins.push_back(o.frame);  // by slot: Particle::origin indexes them
    }
    state_.publish();
    last_publish_ = t;
    stats_.alive.store(count_, std::memory_order_relaxed);
    stats_.publishes.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace fovea
