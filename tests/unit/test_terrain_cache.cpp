// The terrain tile cache (milestone 3 step 6): slots, eviction and the
// memory budget, without a GPU.

#include <gtest/gtest.h>

#include "fovea/render/terrain_cache.hpp"
#include "support/alloc_counter.hpp"

namespace fovea {
namespace {

constexpr TileKey kRoot{12, 2120, 2836};

TileInfo info_for(const TileKey& key, std::size_t bytes = 100) {
    TileInfo i;
    i.key = key;
    i.lod.error_m = 1.0;
    i.bytes = bytes;
    return i;
}

// Reserves and delivers a tile.
std::uint32_t load(TileCache& c, const TileKey& key, std::uint64_t frame) {
    const auto s = c.reserve(key, 100, frame);
    EXPECT_TRUE(s.has_value()) << to_string(key);
    c.arrived(s.value_or(0), info_for(key), frame);
    return s.value_or(0);
}

TEST(TerrainCache, ChildrenLinkUnderTheirParentAsTheyArrive) {
    TileCache c(8, 1 << 20);
    const auto root = c.add_root(info_for(kRoot));
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(c.roots().size(), 1U);
    const auto kids = children(kRoot);
    // Not under a tile that isn't resident.
    EXPECT_FALSE(c.reserve(children(kids[0])[0], 100, 1).has_value());
    const auto s = c.reserve(kids[2], 100, 1);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(c.find(kids[2]), s);
    EXPECT_FALSE(c.reserve(kids[2], 100, 1).has_value());  // once
    EXPECT_EQ(c.lod()[*root].children[2], kNoTile);        // not until it arrives
    EXPECT_EQ(c.stats().loading, 1U);
    c.arrived(*s, info_for(kids[2]), 2);
    EXPECT_EQ(c.lod()[*root].children[2], *s);
    EXPECT_TRUE(c.resident(*s));
    EXPECT_EQ(c.stats().resident, 2U);
    EXPECT_EQ(c.stats().loading, 0U);
}

TEST(TerrainCache, EvictsTheLeastRecentlyDrawnLeafOnlyOnceItsFramesAreDone) {
    // The root, two of its children and a grandchild under each: full.
    TileCache c(5, 1 << 20, 1);
    const auto root = *c.add_root(info_for(kRoot));
    const auto kids = children(kRoot);
    const std::uint32_t k0 = load(c, kids[0], 1);
    const std::uint32_t k1 = load(c, kids[1], 1);
    const std::uint32_t g0 = load(c, children(kids[0])[0], 1);
    const std::uint32_t g1 = load(c, children(kids[1])[0], 1);
    // Frame 10 draws g0 (and so k0 and the root); g1 was last drawn in 1.
    c.touch(g0, 10);
    // Another tile under k0: no free slot, so g1 goes (the least recently
    // drawn leaf; k1 is its parent, so not a leaf); its slot isn't free
    // until frame 11 is done.
    const TileKey more = children(kids[0])[1];
    EXPECT_FALSE(c.reserve(more, 100, 11).has_value());
    EXPECT_FALSE(c.find(children(kids[1])[0]).has_value());
    EXPECT_EQ(c.lod()[k1].children[0], kNoTile);
    EXPECT_EQ(c.stats().evictions, 1U);
    EXPECT_EQ(c.stats().retired, 1U);
    c.frames_completed(10);
    EXPECT_FALSE(c.reserve(more, 100, 12).has_value());  // still not free...
    EXPECT_EQ(c.stats().evictions, 1U);                  // ...and asking again doesn't evict more
    EXPECT_TRUE(c.find(kids[1]).has_value());
    c.frames_completed(11);
    const auto m = c.reserve(more, 100, 12);
    ASSERT_TRUE(m.has_value());
    EXPECT_EQ(*m, g1);  // the freed slot
    (void)root;
    (void)k0;
}

TEST(TerrainCache, DrawingATileKeepsItsSiblings) {
    // A parent splits only with all four children: those outside the view
    // (not drawn) stay while one of them is drawn.
    TileCache c(6, 1 << 20, 1);
    (void)c.add_root(info_for(kRoot));
    const auto kids = children(kRoot);
    std::array<std::uint32_t, 4> s{};
    for (std::size_t i = 0; i < 4; ++i) {
        s[i] = load(c, kids[i], 1);
    }
    c.touch(s[0], 20);                                                  // only kids[0] is in view
    EXPECT_TRUE(c.reserve(children(kids[0])[0], 100, 21).has_value());  // the free slot
    EXPECT_FALSE(c.reserve(children(kids[0])[1], 100, 21).has_value());
    EXPECT_EQ(c.stats().evictions, 0U);  // none of kids[1..3] went
}

TEST(TerrainCache, NeverEvictsRootsParentsOrWhatWasJustDrawn) {
    TileCache c(3, 1 << 20, 1);
    (void)c.add_root(info_for(kRoot));
    const auto kids = children(kRoot);
    const std::uint32_t a = load(c, kids[0], 1);
    // kids[0] has a child loading: it can't go, and the root never does.
    (void)c.reserve(children(kids[0])[0], 100, 1);
    EXPECT_FALSE(c.reserve(kids[1], 100, 50).has_value());
    EXPECT_TRUE(c.find(kids[0]).has_value());
    EXPECT_EQ(c.stats().evictions, 0U);

    TileCache d(2, 1 << 20, 1);
    (void)d.add_root(info_for(kRoot));
    const std::uint32_t b = load(d, kids[0], 1);
    d.touch(b, 9);  // drawn in the frame before
    EXPECT_FALSE(d.reserve(kids[1], 100, 10).has_value());
    EXPECT_EQ(d.stats().evictions, 0U);
    EXPECT_FALSE(d.reserve(kids[1], 100, 11).has_value());  // evicts it now...
    EXPECT_EQ(d.stats().evictions, 1U);
    (void)a;
}

TEST(TerrainCache, KeepsWhatWasUsedInTheLastSecond) {
    // With the default window, a tile drawn 59 frames ago stays; 61, goes.
    TileCache c(2, 1 << 20);
    (void)c.add_root(info_for(kRoot));
    const auto kids = children(kRoot);
    const std::uint32_t a = load(c, kids[0], 1);
    c.touch(a, 100);
    EXPECT_FALSE(c.reserve(kids[1], 100, 159).has_value());
    EXPECT_EQ(c.stats().evictions, 0U);
    EXPECT_FALSE(c.reserve(kids[1], 100, 161).has_value());
    EXPECT_EQ(c.stats().evictions, 1U);
}

TEST(TerrainCache, KeepsWithinTheBudget) {
    // Room for the root (100 bytes) and two more tiles.
    TileCache c(16, 300, 1);
    (void)c.add_root(info_for(kRoot));
    const auto kids = children(kRoot);
    (void)load(c, kids[0], 1);
    (void)load(c, kids[1], 1);
    EXPECT_EQ(c.stats().bytes, 300U);
    // A third evicts one of them to stay in budget.
    EXPECT_FALSE(c.reserve(kids[2], 100, 6).has_value());
    EXPECT_EQ(c.stats().evictions, 1U);
    c.frames_completed(6);
    EXPECT_TRUE(c.reserve(kids[2], 100, 7).has_value());
    EXPECT_LE(c.stats().bytes, 300U);
    // A tile's real size replaces its estimate when it arrives.
    TileCache d(16, 1000);
    (void)d.add_root(info_for(kRoot));
    const auto s = *d.reserve(kids[0], 400, 1);
    EXPECT_EQ(d.stats().bytes, 500U);
    d.arrived(s, info_for(kids[0], 150), 1);
    EXPECT_EQ(d.stats().bytes, 250U);
}

TEST(TerrainCache, FailedTilesAreNotAskedForAgainAndCancelledOnesAre) {
    TileCache c(8, 1 << 20);
    (void)c.add_root(info_for(kRoot));
    const auto kids = children(kRoot);
    const auto s = *c.reserve(kids[0], 100, 1);
    c.failed_to_load(s);
    EXPECT_TRUE(c.failed(kids[0]));
    EXPECT_FALSE(c.reserve(kids[0], 100, 2).has_value());
    EXPECT_EQ(c.stats().failures, 1U);
    const auto t = *c.reserve(kids[1], 100, 2);
    c.cancel(t);
    EXPECT_FALSE(c.failed(kids[1]));
    EXPECT_TRUE(c.reserve(kids[1], 100, 3).has_value());
    EXPECT_EQ(c.stats().loading, 1U);
    EXPECT_EQ(c.stats().bytes, 200U);
}

TEST(TerrainCache, AllocatesNothingOnceMade) {
    TileCache c(64, 1 << 20);
    (void)c.add_root(info_for(kRoot));
    const auto kids = children(kRoot);
    const test::AllocationCounter counter;
    for (std::uint64_t frame = 1; frame < 200; ++frame) {
        for (const TileKey& k : kids) {
            if (!c.find(k)) {
                if (const auto s = c.reserve(k, 100, frame)) {
                    c.arrived(*s, info_for(k), frame);
                }
            }
            for (const TileKey& g : children(k)) {
                if (const auto s = c.reserve(g, 100, frame)) {
                    if (frame % 3 == 0) {
                        c.failed_to_load(*s);
                    } else {
                        c.arrived(*s, info_for(g), frame);
                    }
                }
            }
        }
        c.frames_completed(frame - 1);
    }
    EXPECT_EQ(counter.count(), 0U);
}

TEST(TerrainCache, LookupsStayFastAfterManyEvictions) {
    // A small cache (8 slots, a 32-bucket index) through which hundreds of
    // different tiles pass, thousands of times: entries removed from the
    // index must leave it as it was (tombstones once only ever accumulated,
    // and a lookup of a missing tile would eventually never end).
    TileCache c(8, 1 << 20, 1);
    (void)c.add_root(info_for(kRoot));
    std::uint64_t frame = 1;
    int lost = 0;
    // Load a tile under the root, then each of its descendants in turn,
    // a level at a time, evicting as it goes.
    const auto bring = [&](const TileKey& k) {
        for (int attempt = 0; attempt < 8 && !c.find(k); ++attempt) {
            if (const auto s = c.reserve(k, 100, frame)) {
                c.arrived(*s, info_for(k), frame);
                c.touch(*s, frame);
            }
            c.frames_completed(frame);
            frame += 3;
            // Every tile still there is found where it is, all along.
            for (std::uint32_t s = 0; s < c.capacity(); ++s) {
                if (c.resident(s) && c.find(c.info(s).key) != s) {
                    ++lost;
                }
            }
        }
    };
    for (int sweep = 0; sweep < 30; ++sweep) {
        for (const TileKey& a : children(kRoot)) {
            bring(a);
            for (const TileKey& b : children(a)) {
                bring(b);
                for (const TileKey& d : children(b)) {
                    bring(d);
                    for (const TileKey& e : children(d)) {
                        bring(e);
                    }
                }
            }
        }
    }
    EXPECT_GT(c.stats().evictions, 200U);
    EXPECT_FALSE(c.find({20, 1, 1}).has_value());  // returns at all
    EXPECT_EQ(lost, 0);
}

}  // namespace
}  // namespace fovea
