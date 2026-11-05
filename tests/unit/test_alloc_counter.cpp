#include <memory>

#include <gtest/gtest.h>

#include "support/alloc_counter.hpp"

namespace fovea::test {
namespace {

// Makes a pointer visible to the compiler as used. Since C++14 an unobserved
// new/delete pair may be elided (Clang does this at -O1), and then nothing is
// allocated, so there would be nothing to count. A volatile store works on
// every compiler, where inline asm would not on MSVC x64.
const void* volatile g_escaped = nullptr;

void escape(const void* p) {
    g_escaped = p;
}

// The zero-allocation tests are only meaningful if the counter really sees
// allocations.
TEST(AllocationCounter, CountsHeapAllocations) {
    AllocationCounter counter;
    // Not std::vector: MSVC debug builds give each one a hidden extra
    // allocation for iterator checking.
    auto p = std::make_unique<int>(1);
    auto a = std::make_unique<double[]>(16);  // NOLINT(modernize-avoid-c-arrays)
    escape(p.get());
    escape(a.get());
    EXPECT_EQ(counter.count(), 2U);
}

TEST(AllocationCounter, CountsAlignedAllocations) {
    struct alignas(128) Wide {
        double x = 0.0;
    };
    AllocationCounter counter;
    auto p = std::make_unique<Wide>();
    escape(p.get());
    EXPECT_EQ(counter.count(), 1U);
}

TEST(AllocationCounter, IgnoresAllocationsOutsideItsScope) {
    auto before = std::make_unique<int>(1);
    escape(before.get());
    AllocationCounter counter;
    EXPECT_EQ(counter.count(), 0U);
}

}  // namespace
}  // namespace fovea::test
