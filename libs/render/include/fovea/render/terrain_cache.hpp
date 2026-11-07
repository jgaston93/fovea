#pragma once

// Which terrain tiles are on the GPU (milestone 3 step 6): a fixed number
// of slots, each a tile resident, loading, or retired (evicted, waiting
// for the frames that drew it to finish). The render thread owns it: it
// picks tiles from the resident tree (terrain_lod.hpp), reserves slots for
// the children it wants (evicting the least recently drawn tiles that
// nothing below depends on), and links tiles in as the paging thread
// delivers them. Roots are pinned, so some tile always covers the ground.
// CPU only and allocation-free after construction.

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "fovea/core/math.hpp"
#include "fovea/render/terrain_lod.hpp"
#include "fovea/terrain/tile_key.hpp"

namespace fovea {

// What the render thread needs of a loaded tile.
struct TileInfo {
    TileKey key;
    LodTile lod;  // its children are filled in by the cache
    DVec3 origin_ecef{0.0};
    float skirt_m = 0.0F;
    std::size_t bytes = 0;  // GPU memory it takes
};

struct TileCacheStats {
    std::uint32_t resident = 0;
    std::uint32_t loading = 0;
    std::uint32_t retired = 0;
    std::size_t bytes = 0;  // resident, loading (as estimated) and retired
    std::uint64_t evictions = 0;
    std::uint64_t failures = 0;
};

class TileCache {
public:
    // `capacity` slots and `budget_bytes` of GPU memory, at most. A tile
    // drawn or wanted in the last `keep_frames` frames is never evicted
    // (a second at 60 Hz): when the views want more than fits, requests
    // wait (and show as misses) rather than evicting what the next frame
    // will ask for again.
    TileCache(std::uint32_t capacity, std::size_t budget_bytes, std::uint32_t keep_frames = 60);

    std::uint32_t capacity() const { return static_cast<std::uint32_t>(slots_.size()); }

    // A root, loaded up front: resident and never evicted. Returns its
    // slot, or nothing when the cache is full.
    std::optional<std::uint32_t> add_root(const TileInfo& info);

    // The slot holding (or loading) a tile.
    std::optional<std::uint32_t> find(const TileKey& key) const;
    // A tile that failed to load: never asked for again.
    bool failed(const TileKey& key) const;

    // A slot for a tile to load (its parent must be resident), taking
    // `estimate_bytes` against the budget until it arrives. When the cache
    // is full or over budget, evicts the least recently drawn tiles that are
    // neither roots, nor drawn in the last keep_frames, nor parents of anything
    // resident or loading (nor this tile's parent), but only as far as
    // tiles already retiring won't make room. Nothing when there is no room
    // yet: an evicted tile's room comes back once its frames finish.
    std::optional<std::uint32_t> reserve(const TileKey& key, std::size_t estimate_bytes,
                                         std::uint64_t frame);
    // A reserved slot's tile arrived: resident, linked under its parent.
    void arrived(std::uint32_t slot, const TileInfo& info, std::uint64_t frame);
    // A reserved slot's tile can't be loaded: the slot is free again (no GPU
    // use) and the tile is remembered as failed.
    void failed_to_load(std::uint32_t slot);
    // A reservation never sent (a full queue): the slot is free again.
    void cancel(std::uint32_t slot);

    // A tile drawn in `frame`: it, its ancestors and all their siblings stay.
    void touch(std::uint32_t slot, std::uint64_t frame);
    // Every frame up to and including `frame` has finished on the GPU:
    // tiles retired before then free their slots.
    void frames_completed(std::uint64_t frame);

    // For select_tiles: per slot (children kNoTile where not resident;
    // slots not resident are never reached from the roots).
    std::span<const LodTile> lod() const { return lod_; }
    std::span<const std::uint32_t> roots() const { return roots_; }
    const TileInfo& info(std::uint32_t slot) const { return slots_[slot].info; }
    bool resident(std::uint32_t slot) const { return slots_[slot].state == State::resident; }

    const TileCacheStats& stats() const { return stats_; }

private:
    enum class State : std::uint8_t { free, loading, resident, retired };
    struct Slot {
        State state = State::free;
        TileInfo info;
        std::uint32_t parent = kNoTile;
        std::uint64_t last_used = 0;
        std::uint64_t retired_after = 0;  // free once this frame completes
        std::size_t charged = 0;          // bytes counted against the budget
        bool root = false;
    };

    std::optional<std::uint32_t> take_free();
    bool evict_one(std::uint64_t frame, std::uint32_t keep);
    bool depended_on(std::uint32_t slot) const;
    void index_put(const TileKey& key, std::uint32_t slot);
    void index_erase(const TileKey& key);
    std::size_t index_find(const TileKey& key) const;  // its bucket, or kEmpty's

    std::vector<Slot> slots_;
    std::vector<LodTile> lod_;
    std::vector<std::uint32_t> roots_;
    std::vector<std::uint32_t> free_;  // a stack of free slots
    std::size_t budget_bytes_;
    std::uint32_t keep_frames_;
    std::size_t retired_bytes_ = 0;  // of stats_.bytes: freed once their frames finish
    TileCacheStats stats_;
    std::uint64_t last_frame_ = 0;
    // Key to slot, open addressing (twice the slots, so never full).
    struct Bucket {
        TileKey key;
        std::uint32_t slot = kNoTile;  // kNoTile: empty; kTombstone: erased
    };
    std::vector<Bucket> index_;
    // Tiles that failed to load, likewise.
    std::vector<TileKey> failed_;
    std::size_t failed_count_ = 0;
};

}  // namespace fovea
