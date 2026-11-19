#include "fovea/net/puff_adapter.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

#include "fovea/core/geodesy.hpp"

namespace fovea {

namespace {

constexpr int kMaxDatagramsPerPoll = 4096;
constexpr auto kLossWindow = std::chrono::seconds(1);
// A sequence further ahead than this is a discontinuity (a corrupt number),
// not that many datagrams lost: counting starts again from it.
constexpr std::int64_t kMaxSequenceGap = 65536;

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
    const puff::HeaderResult read = puff::read_header(d, h);
    if (read == puff::HeaderResult::rejected) {
        stats_.rejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Source* s = source_for(h, t);
    if (s == nullptr) {
        stats_.rejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // A skipped type still took a sequence number: counted, so it isn't a loss.
    count_sequence(*s, h.sequence);
    if (read == puff::HeaderResult::unknown_type) {
        stats_.skipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto source = static_cast<std::uint32_t>(s - sources_.data());
    if (h.type == static_cast<std::uint8_t>(puff::Type::origin)) {
        apply_origin(source, h, puff::read_origin(d, h), t);
    } else {
        apply_puffs(source, d, h, t);
    }
}

PuffAdapter::Source* PuffAdapter::source_for(const puff::Header& h, LocalTime t) noexcept {
    Source* free = nullptr;
    for (std::size_t i = 0; i < sources_.size(); ++i) {
        Source& s = sources_.at(i);
        if (!s.used) {
            free = free != nullptr ? free : &s;
            continue;
        }
        if (s.id != h.source_id) {
            continue;
        }
        if (s.session != h.session) {
            if (s.has_previous && h.session == s.previous_session) {
                return nullptr;  // a stray from before the restart: it mustn't reset the new one
            }
            // Restarted: its time starts again, so its old puffs and origins
            // would win against everything new.
            close_window(s);
            reset_source(static_cast<std::uint32_t>(i));
            s.has_previous = true;
            s.previous_session = s.session;
            s.session = h.session;
            stats_.sessions.fetch_add(1, std::memory_order_relaxed);
        }
        s.last_heard = t;
        return &s;
    }
    if (free == nullptr) {
        return nullptr;
    }
    *free = Source{};
    free->used = true;
    free->id = h.source_id;
    free->session = h.session;
    free->last_heard = t;
    stats_.sessions.fetch_add(1, std::memory_order_relaxed);
    return free;
}

void PuffAdapter::reset_source(std::uint32_t index) noexcept {
    for (std::size_t i = 0; i < count_;) {
        if (held_[i].source == index) {
            remove(i);  // the last moves into i: look at i again
        } else {
            ++i;
        }
    }
    for (OriginSlot& o : origins_) {
        if (o.used && o.source == index) {
            o.used = false;
        }
    }
    sources_.at(index).counting = false;
}

void PuffAdapter::count_sequence(Source& s, std::uint32_t sequence) noexcept {
    if (!s.counting) {
        s.counting = true;
        s.base = sequence;
        s.highest = 0;
        s.received = 1;
        return;
    }
    const auto d = static_cast<std::int64_t>(static_cast<std::int32_t>(sequence - s.base));
    if (d < 0) {
        return;  // behind the window: an earlier window counted it lost; it's late, not new
    }
    if (d - s.highest > kMaxSequenceGap) {
        s.base = sequence;  // a discontinuity: start again from it, counting no loss
        s.highest = 0;
        s.received = 1;
        return;
    }
    s.highest = std::max(s.highest, d);
    ++s.received;
}

void PuffAdapter::close_window(Source& s) noexcept {
    if (!s.counting) {
        return;
    }
    const auto expected = static_cast<std::uint64_t>(s.highest + 1);
    if (expected > s.received) {
        stats_.lost.fetch_add(expected - s.received, std::memory_order_relaxed);
    }
    // The next window expects the sequence after this one's highest, so a
    // loss across the boundary is counted too.
    s.base += static_cast<std::uint32_t>(s.highest + 1);
    s.highest = -1;
    s.received = 0;
}

int PuffAdapter::find_origin(std::uint32_t source, std::uint32_t id) const noexcept {
    for (std::size_t i = 0; i < origins_.size(); ++i) {
        if (origins_.at(i).used && origins_.at(i).source == source && origins_.at(i).id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void PuffAdapter::apply_origin(std::uint32_t source, const puff::Header& h, const puff::Origin& o,
                               LocalTime t) noexcept {
    if (!std::isfinite(o.latitude_rad) || !std::isfinite(o.longitude_rad) ||
        !std::isfinite(o.height_msl_m)) {
        stats_.rejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    int slot = find_origin(source, h.origin_id);
    for (std::size_t i = 0; i < origins_.size() && slot < 0; ++i) {
        if (!origins_.at(i).used) {
            slot = static_cast<int>(i);
            origins_.at(i).refs = 0;
        }
    }
    if (slot < 0) {
        stats_.origins_full.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    constexpr double kDeg = 180.0 / std::numbers::pi;
    const double lat = o.latitude_rad * kDeg;
    const double lon = o.longitude_rad * kDeg;
    OriginSlot& s = origins_.at(static_cast<std::size_t>(slot));
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
    const int found = find_origin(source, h.origin_id);
    if (found < 0) {
        stats_.unknown_origin.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto slot = static_cast<std::uint16_t>(found);
    origins_.at(slot).last_used = t;
    const LocalTime fade_from =
        t + std::chrono::duration_cast<Clock::duration>(std::chrono::microseconds(h.hold_us));
    const bool frozen = (h.flags & puff::kFlagFrozen) != 0;
    Source& src = sources_.at(source);
    for (std::size_t i = 0; i < h.count; ++i) {
        const puff::Puff r = puff::read_puff(d, h, i);
        if (!finite_puff(r)) {
            stats_.rejected_records.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        std::size_t at = find(source, r.id);
        if (at != npos) {
            if (held_[at].sim_time_ns > h.sim_time_ns) {
                stats_.late.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            --origins_.at(held_[at].p.origin).refs;
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
            ++src.alive;
        }
        Held& e = held_[at];
        e.sim_time_ns = h.sim_time_ns;
        e.p.position = Vec3(r.east, r.north, r.up);
        e.p.velocity = Vec3(r.v_east, r.v_north, r.v_up);
        e.p.radius = r.radius;
        e.p.bulk_density = r.bulk_density;
        e.p.received = t;
        e.p.fade_from = fade_from;
        e.p.origin = slot;
        e.p.frozen = frozen;
        ++origins_.at(slot).refs;
        stats_.records.fetch_add(1, std::memory_order_relaxed);
    }
}

std::size_t PuffAdapter::find_slot(std::uint32_t source, std::uint32_t id) const noexcept {
    const std::size_t mask = index_.size() - 1;
    for (std::size_t i = hash_of(source, id) & mask;; i = (i + 1) & mask) {
        const std::uint32_t v = index_[i];
        if (v == 0) {
            return npos;
        }
        const Held& e = held_[v - 1];
        if (e.source == source && e.id == id) {
            return i;
        }
    }
}

std::size_t PuffAdapter::find(std::uint32_t source, std::uint32_t id) const noexcept {
    const std::size_t slot = find_slot(source, id);
    return slot == npos ? npos : index_[slot] - 1;
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

// Linear probing's backward-shift deletion: no tombstones, so the index never
// needs rebuilding. An entry after the hole moves into it unless its home
// lies cyclically in (hole, entry].
void PuffAdapter::index_erase(std::size_t slot) noexcept {
    const std::size_t mask = index_.size() - 1;
    std::size_t hole = slot;
    index_[hole] = 0;
    for (std::size_t j = (hole + 1) & mask; index_[j] != 0; j = (j + 1) & mask) {
        const Held& e = held_[index_[j] - 1];
        const std::size_t home = hash_of(e.source, e.id) & mask;
        const bool stays = hole <= j ? (hole < home && home <= j) : (hole < home || home <= j);
        if (!stays) {
            index_[hole] = index_[j];
            index_[j] = 0;
            hole = j;
        }
    }
}

// Removes the particle at `at`: the last moves into its place (order doesn't
// matter), its index entry repointed. Constant time on average.
void PuffAdapter::remove(std::size_t at) noexcept {
    Held& e = held_[at];
    --origins_.at(e.p.origin).refs;
    --sources_.at(e.source).alive;
    index_erase(find_slot(e.source, e.id));
    const std::size_t last = count_ - 1;
    if (at != last) {
        index_[find_slot(held_[last].source, held_[last].id)] = static_cast<std::uint32_t>(at + 1);
        held_[at] = held_[last];
    }
    --count_;
}

void PuffAdapter::compact(LocalTime t) noexcept {
    const auto fade = std::chrono::duration_cast<Clock::duration>(options_.fade);
    for (std::size_t i = 0; i < count_;) {
        if (t >= held_[i].p.fade_from + fade) {
            remove(i);  // the last moves into i: look at i again
        } else {
            ++i;
        }
    }
    for (OriginSlot& o : origins_) {
        if (o.used && o.refs == 0 && t - o.last_used > kForgetOrigin) {
            o.used = false;
        }
    }
    for (std::size_t i = 0; i < sources_.size(); ++i) {
        Source& s = sources_.at(i);
        if (s.used && s.alive == 0 && t - s.last_heard > kForget) {
            reset_source(static_cast<std::uint32_t>(i));
            s.used = false;
        }
    }
}

void PuffAdapter::publish(LocalTime t) noexcept {
    if (t - window_start_ >= kLossWindow) {
        for (Source& s : sources_) {
            if (s.used) {
                close_window(s);
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
