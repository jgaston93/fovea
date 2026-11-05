#pragma once

#include <cstdint>

// Counts heap allocations made on the current thread while an
// AllocationCounter is alive. Backed by replacements of the global
// operator new family in alloc_counter.cpp; used to enforce the
// no-allocation rule on the render path (architecture.md, "Render path
// rules").
namespace fovea::test {

class AllocationCounter {
public:
    AllocationCounter();
    ~AllocationCounter();
    AllocationCounter(const AllocationCounter&) = delete;
    AllocationCounter& operator=(const AllocationCounter&) = delete;

    std::uint64_t count() const;

private:
    std::uint64_t start_;
    bool was_armed_;
};

}  // namespace fovea::test
