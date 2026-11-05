#include <atomic>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "fovea/core/containers.hpp"
#include "support/alloc_counter.hpp"

namespace fovea {
namespace {

using test::AllocationCounter;

TEST(FixedArray, PushPastCapacityFails) {
    FixedArray<int, 3> a;
    EXPECT_TRUE(a.push_back(1));
    EXPECT_TRUE(a.push_back(2));
    EXPECT_TRUE(a.push_back(3));
    EXPECT_FALSE(a.push_back(4));
    EXPECT_TRUE(a.full());
    EXPECT_EQ(a.size(), 3U);
    EXPECT_EQ(a[2], 3);
    a.pop_back();
    EXPECT_EQ(a.span().size(), 2U);
    a.clear();
    EXPECT_TRUE(a.empty());
}

TEST(FrameArena, AllocationsAreAlignedAndResetFreesThem) {
    FrameArena arena(1024);
    auto bytes = arena.allocate_array<std::uint8_t>(3);
    ASSERT_EQ(bytes.size(), 3U);
    auto doubles = arena.allocate_array<double>(4);
    ASSERT_EQ(doubles.size(), 4U);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(doubles.data()) % alignof(double), 0U);
    EXPECT_EQ(doubles[3], 0.0);  // value-initialized
    void* wide = arena.allocate(16, 64);
    ASSERT_NE(wide, nullptr);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(wide) % 64, 0U);

    const std::size_t used = arena.used();
    arena.reset();
    EXPECT_EQ(arena.used(), 0U);
    EXPECT_EQ(arena.high_water(), used);
}

TEST(FrameArena, FullArenaReturnsNothing) {
    FrameArena arena(64);
    EXPECT_EQ(arena.allocate_array<double>(8).size(), 8U);
    EXPECT_TRUE(arena.allocate_array<double>(1).empty());
    EXPECT_EQ(arena.allocate(1, 1), nullptr);
}

TEST(FrameArena, HugeArrayRequestReturnsNothing) {
    FrameArena arena(64);
    // sizeof(double) * n would wrap around to 8 bytes.
    EXPECT_TRUE(arena.allocate_array<double>(SIZE_MAX / 8 + 2).empty());
    EXPECT_EQ(arena.used(), 0U);
}

TEST(SpscRing, FifoOrderAndCapacity) {
    SpscRing<int, 4> ring;
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(ring.try_push(i));
    }
    EXPECT_FALSE(ring.try_push(99));
    for (int i = 0; i < 4; ++i) {
        EXPECT_EQ(ring.try_pop(), i);
    }
    EXPECT_FALSE(ring.try_pop().has_value());
}

TEST(SpscRing, TwoThreadsPassEveryValueInOrder) {
    constexpr int kCount = 200'000;
    SpscRing<int, 64> ring;
    std::thread producer([&] {
        for (int i = 0; i < kCount;) {
            if (ring.try_push(i)) {
                ++i;
            }
        }
    });
    // Checks count instead of ASSERT: returning early would leave the
    // producer spinning on a full ring and the thread unjoined.
    int received = 0;
    int out_of_order = 0;
    while (received < kCount) {
        if (auto v = ring.try_pop()) {
            out_of_order += (*v != received) ? 1 : 0;
            ++received;
        }
    }
    producer.join();
    EXPECT_EQ(out_of_order, 0);
}

TEST(MpmcQueue, FifoOrderAndCapacity) {
    MpmcQueue<int> q(4);
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(q.try_push(i));
    }
    EXPECT_FALSE(q.try_push(99));
    int v = -1;
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_FALSE(q.try_pop(v));
}

TEST(MpmcQueue, ManyProducersLoseNothingAndKeepEachOrder) {
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 50'000;
    MpmcQueue<std::uint64_t> q(256);
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&q, p] {
            for (int i = 0; i < kPerProducer;) {
                const auto value =
                    (static_cast<std::uint64_t>(p) << 32) | static_cast<std::uint32_t>(i);
                if (q.try_push(value)) {
                    ++i;
                }
            }
        });
    }
    std::vector<std::int64_t> last(kProducers, -1);
    int received = 0;
    int bad = 0;
    std::uint64_t v = 0;
    while (received < kProducers * kPerProducer) {
        if (q.try_pop(v)) {
            const auto p = static_cast<std::size_t>(v >> 32);
            const auto i = static_cast<std::int64_t>(v & 0xffff'ffffU);
            if (p >= last.size() || i != last[p] + 1) {
                ++bad;
            } else {
                last[p] = i;
            }
            ++received;
        }
    }
    for (auto& t : producers) {
        t.join();
    }
    EXPECT_EQ(bad, 0);
}

TEST(MpmcQueue, RejectsACapacityThatIsNotAPowerOfTwo) {
    EXPECT_THROW(MpmcQueue<int>(0), std::invalid_argument);
    EXPECT_THROW(MpmcQueue<int>(1), std::invalid_argument);
    EXPECT_THROW(MpmcQueue<int>(1000), std::invalid_argument);
    EXPECT_NO_THROW(MpmcQueue<int>(1024));
}

TEST(TripleBuffer, ReaderSeesTheLatestPublishedValue) {
    TripleBuffer<int> tb;
    EXPECT_FALSE(tb.has_new());
    EXPECT_EQ(tb.read(), 0);
    tb.write_slot() = 1;
    tb.publish();
    tb.write_slot() = 2;
    tb.publish();
    EXPECT_TRUE(tb.has_new());
    EXPECT_EQ(tb.read(), 2);  // 1 was superseded
    EXPECT_FALSE(tb.has_new());
    EXPECT_EQ(tb.read(), 2);  // unchanged until the next publish
}

// A snapshot whose fields must always agree; a torn read breaks that.
struct Snapshot {
    std::uint64_t sequence = 0;
    std::uint64_t doubled = 0;
    std::uint64_t pattern[14] =
        {};  // NOLINT(modernize-avoid-c-arrays): padding to a cache line pair
};

TEST(TripleBuffer, ConcurrentReaderNeverSeesATornOrOlderValue) {
    constexpr std::uint64_t kCount = 300'000;
    TripleBuffer<Snapshot> tb;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (std::uint64_t s = 1; s <= kCount; ++s) {
            Snapshot& slot = tb.write_slot();
            slot.sequence = s;
            slot.doubled = 2 * s;
            for (auto& word : slot.pattern) {
                word = s;
            }
            tb.publish();
        }
        done.store(true, std::memory_order_release);
    });
    std::uint64_t last = 0;
    int older = 0;
    int torn = 0;
    for (;;) {
        const bool finished = done.load(std::memory_order_acquire);
        const Snapshot& s = tb.read();
        older += (s.sequence < last) ? 1 : 0;
        bool consistent = s.doubled == 2 * s.sequence;
        for (auto word : s.pattern) {
            consistent = consistent && word == s.sequence;
        }
        torn += consistent ? 0 : 1;
        last = s.sequence;
        if (finished) {
            break;
        }
    }
    writer.join();
    EXPECT_EQ(older, 0);
    EXPECT_EQ(torn, 0);
    EXPECT_EQ(tb.read().sequence, kCount);
}

TEST(Containers, HandoffAllocatesNothing) {
    SpscRing<int, 8> ring;
    MpmcQueue<int> q(8);
    TripleBuffer<Snapshot> tb;
    FrameArena arena(256);
    FixedArray<int, 4> fixed;
    AllocationCounter counter;
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(ring.try_push(i));
        ASSERT_TRUE(ring.try_pop().has_value());
        int v = 0;
        ASSERT_TRUE(q.try_push(i));
        ASSERT_TRUE(q.try_pop(v));
        tb.write_slot().sequence = static_cast<std::uint64_t>(i);
        tb.publish();
        ASSERT_EQ(tb.read().sequence, static_cast<std::uint64_t>(i));
        ASSERT_FALSE(arena.allocate_array<double>(8).empty());
        arena.reset();
        ASSERT_TRUE(fixed.push_back(i));
        fixed.clear();
    }
    EXPECT_EQ(counter.count(), 0U);
}

}  // namespace
}  // namespace fovea
