#include "fovea/render/terrain_cache.hpp"

#include <algorithm>
#include <bit>
#include <functional>

namespace fovea {

namespace {

constexpr std::uint32_t kTombstone = kNoTile - 1;

}  // namespace

TileCache::TileCache(std::uint32_t capacity, std::size_t budget_bytes, std::uint32_t keep_frames)
    : slots_(capacity),
      lod_(capacity),
      budget_bytes_(budget_bytes),
      keep_frames_(std::max(keep_frames, 1U)),
      index_(std::bit_ceil(std::size_t{capacity} * 2 + 2)),
      failed_(capacity) {
    roots_.reserve(capacity);
    free_.reserve(capacity);
    for (std::uint32_t s = capacity; s-- > 0;) {
        free_.push_back(s);  // slot 0 comes off first
    }
}

std::size_t TileCache::index_find(const TileKey& key) const {
    const std::size_t mask = index_.size() - 1;
    for (std::size_t i = std::hash<TileKey>{}(key)&mask;; i = (i + 1) & mask) {
        const Bucket& b = index_[i];
        if (b.slot == kNoTile || (b.slot != kTombstone && b.key == key)) {
            return i;
        }
    }
}

void TileCache::index_put(const TileKey& key, std::uint32_t slot) {
    const std::size_t mask = index_.size() - 1;
    std::size_t i = std::hash<TileKey>{}(key)&mask;
    while (index_[i].slot != kNoTile && index_[i].slot != kTombstone) {
        i = (i + 1) & mask;
    }
    index_[i] = {key, slot};
}

void TileCache::index_erase(const TileKey& key) {
    const std::size_t i = index_find(key);
    if (index_[i].slot != kNoTile) {
        index_[i].slot = kTombstone;
    }
}

std::optional<std::uint32_t> TileCache::find(const TileKey& key) const {
    const Bucket& b = index_[index_find(key)];
    return b.slot == kNoTile ? std::nullopt : std::optional(b.slot);
}

bool TileCache::failed(const TileKey& key) const {
    const auto end = failed_.begin() + static_cast<std::ptrdiff_t>(failed_count_);
    return std::find(failed_.begin(), end, key) != end;
}

std::optional<std::uint32_t> TileCache::take_free() {
    if (free_.empty()) {
        return std::nullopt;
    }
    const std::uint32_t s = free_.back();
    free_.pop_back();
    return s;
}

std::optional<std::uint32_t> TileCache::add_root(const TileInfo& info) {
    const auto s = take_free();
    if (!s) {
        return std::nullopt;
    }
    Slot& slot = slots_[*s];
    slot = {};
    slot.state = State::resident;
    slot.info = info;
    slot.root = true;
    slot.charged = info.bytes;
    lod_[*s] = info.lod;
    lod_[*s].children = {kNoTile, kNoTile, kNoTile, kNoTile};
    roots_.push_back(*s);
    index_put(info.key, *s);
    stats_.bytes += info.bytes;
    ++stats_.resident;
    return s;
}

bool TileCache::depended_on(std::uint32_t s) const {
    // Children resident or loading: a tile's children link to it.
    const auto kids = children(slots_[s].info.key);
    return std::any_of(kids.begin(), kids.end(),
                       [&](const TileKey& c) { return find(c).has_value(); });
}

bool TileCache::evict_one(std::uint64_t frame, std::uint32_t keep) {
    // The least recently drawn tile nothing depends on, not drawn or
    // wanted in the last keep_frames_.
    std::uint32_t best = kNoTile;
    for (std::uint32_t s = 0; s < slots_.size(); ++s) {
        const Slot& slot = slots_[s];
        if (s == keep || slot.state != State::resident || slot.root ||
            slot.last_used + keep_frames_ >= frame ||
            (best != kNoTile && slot.last_used >= slots_[best].last_used) || depended_on(s)) {
            continue;
        }
        best = s;
    }
    if (best == kNoTile) {
        return false;
    }
    Slot& slot = slots_[best];
    if (slot.parent != kNoTile) {
        for (std::uint32_t& c : lod_[slot.parent].children) {
            c = c == best ? kNoTile : c;
        }
    }
    index_erase(slot.info.key);
    slot.state = State::retired;
    // Frames up to this one may have drawn it.
    slot.retired_after = std::max(frame, last_frame_);
    retired_bytes_ += slot.charged;
    --stats_.resident;
    ++stats_.retired;
    ++stats_.evictions;
    return true;
}

std::optional<std::uint32_t> TileCache::reserve(const TileKey& key, std::size_t estimate_bytes,
                                                std::uint64_t frame) {
    last_frame_ = std::max(last_frame_, frame);
    const auto up = parent(key);
    const auto p = up ? find(*up) : std::nullopt;
    if (!p || slots_[*p].state != State::resident || find(key) || failed(key)) {
        return std::nullopt;
    }
    // Evicts only what retiring tiles won't free anyway: a slot, or bytes,
    // once their frames finish. Until then the request waits (asked for
    // again in later frames), rather than evicting more each time.
    while ((free_.empty() && stats_.retired == 0) ||
           stats_.bytes - retired_bytes_ + estimate_bytes > budget_bytes_) {
        if (!evict_one(frame, *p)) {
            return std::nullopt;
        }
    }
    if (free_.empty() || stats_.bytes + estimate_bytes > budget_bytes_) {
        return std::nullopt;  // room once retired tiles' frames finish
    }
    const auto s = take_free();
    if (!s) {
        return std::nullopt;
    }
    Slot& slot = slots_[*s];
    slot = {};
    slot.state = State::loading;
    slot.info.key = key;
    slot.parent = *p;
    slot.last_used = frame;
    slot.charged = estimate_bytes;
    lod_[*s] = {};
    index_put(key, *s);
    stats_.bytes += estimate_bytes;
    ++stats_.loading;
    return s;
}

void TileCache::arrived(std::uint32_t s, const TileInfo& info, std::uint64_t frame) {
    Slot& slot = slots_[s];
    stats_.bytes = stats_.bytes - slot.charged + info.bytes;
    slot.charged = info.bytes;
    slot.info = info;
    slot.state = State::resident;
    slot.last_used = frame;
    lod_[s] = info.lod;
    lod_[s].children = {kNoTile, kNoTile, kNoTile, kNoTile};
    --stats_.loading;
    ++stats_.resident;
    // Under its parent, in the child's place.
    const TileKey& k = info.key;
    const auto kids = children(slots_[slot.parent].info.key);
    for (std::size_t c = 0; c < kids.size(); ++c) {
        if (kids[c] == k) {
            lod_[slot.parent].children[c] = s;
        }
    }
}

void TileCache::failed_to_load(std::uint32_t s) {
    if (failed_count_ < failed_.size()) {
        failed_[failed_count_++] = slots_[s].info.key;
    }
    ++stats_.failures;
    cancel(s);
}

void TileCache::cancel(std::uint32_t s) {
    Slot& slot = slots_[s];
    index_erase(slot.info.key);
    stats_.bytes -= slot.charged;
    --stats_.loading;
    slot = {};
    free_.push_back(s);
}

void TileCache::touch(std::uint32_t s, std::uint64_t frame) {
    last_frame_ = std::max(last_frame_, frame);
    // It, its ancestors, and their siblings: each split above it needs all
    // four children, and those outside the view aren't drawn, so evicting
    // them would undo the split (and load them again).
    for (std::uint32_t t = s; t != kNoTile; t = slots_[t].parent) {
        slots_[t].last_used = frame;
        if (const std::uint32_t p = slots_[t].parent; p != kNoTile) {
            for (const std::uint32_t c : lod_[p].children) {
                if (c != kNoTile) {
                    slots_[c].last_used = frame;
                }
            }
        }
    }
}

void TileCache::frames_completed(std::uint64_t frame) {
    if (stats_.retired == 0) {
        return;
    }
    for (std::uint32_t s = 0; s < slots_.size(); ++s) {
        Slot& slot = slots_[s];
        if (slot.state == State::retired && slot.retired_after <= frame) {
            stats_.bytes -= slot.charged;
            retired_bytes_ -= slot.charged;
            --stats_.retired;
            slot = {};
            free_.push_back(s);
        }
    }
}

}  // namespace fovea
