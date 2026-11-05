#pragma once

// Fixed-capacity containers and lock-free handoff between threads
// (architecture.md, "Threads and data handoff"). All memory is reserved at
// construction; nothing here allocates afterwards.

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace fovea {

// Typical cache-line size; std::hardware_destructive_interference_size is
// not used because GCC warns that it may differ between compilers.
inline constexpr std::size_t kCacheLine = 64;

// Vector-like storage with a fixed capacity and no heap.
template <typename T, std::size_t N>
class FixedArray {
public:
    // Returns false when full.
    bool push_back(const T& value) {
        if (size_ == N) {
            return false;
        }
        items_[size_++] = value;
        return true;
    }

    void pop_back() {
        assert(size_ > 0);
        --size_;
    }

    void clear() noexcept { size_ = 0; }

    T& operator[](std::size_t i) {
        assert(i < size_);
        return items_[i];
    }
    const T& operator[](std::size_t i) const {
        assert(i < size_);
        return items_[i];
    }

    std::size_t size() const noexcept { return size_; }
    static constexpr std::size_t capacity() noexcept { return N; }
    bool empty() const noexcept { return size_ == 0; }
    bool full() const noexcept { return size_ == N; }

    T* begin() noexcept { return items_.data(); }
    T* end() noexcept { return items_.data() + size_; }
    const T* begin() const noexcept { return items_.data(); }
    const T* end() const noexcept { return items_.data() + size_; }
    std::span<T> span() noexcept { return {items_.data(), size_}; }
    std::span<const T> span() const noexcept { return {items_.data(), size_}; }

private:
    std::array<T, N> items_{};
    std::size_t size_ = 0;
};

// Linear allocator reset once per frame: scratch memory for the render path.
class FrameArena {
public:
    explicit FrameArena(std::size_t capacity) : storage_(capacity) {}

    // Returns nullptr when the arena is full.
    void* allocate(std::size_t size, std::size_t align) noexcept {
        void* p = storage_.data() + used_;
        std::size_t space = storage_.size() - used_;
        if (std::align(align, size, p, space) == nullptr) {
            return nullptr;
        }
        used_ = storage_.size() - space + size;
        high_water_ = used_ > high_water_ ? used_ : high_water_;
        return p;
    }

    // Value-initialized array of n trivially destructible T; empty when full.
    template <typename T>
        requires std::is_trivially_destructible_v<T> && std::is_default_constructible_v<T>
    std::span<T> allocate_array(std::size_t n) noexcept {
        void* p = allocate(sizeof(T) * n, alignof(T));
        if (p == nullptr) {
            return {};
        }
        T* first = static_cast<T*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            ::new (static_cast<void*>(first + i)) T{};
        }
        return {first, n};
    }

    void reset() noexcept { used_ = 0; }

    std::size_t used() const noexcept { return used_; }
    std::size_t capacity() const noexcept { return storage_.size(); }
    std::size_t high_water() const noexcept { return high_water_; }

private:
    std::vector<std::byte> storage_;
    std::size_t used_ = 0;
    std::size_t high_water_ = 0;
};

// Single-producer, single-consumer ring of N slots (a power of two).
template <typename T, std::size_t N>
class SpscRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0, "SpscRing size must be a power of two");

public:
    // Producer side. Returns false when full.
    bool try_push(T value) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_.load(std::memory_order_acquire) == N) {
            return false;
        }
        slots_[tail & (N - 1)] = std::move(value);
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Consumer side. Empty when there is nothing to take.
    std::optional<T> try_pop() {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return std::nullopt;
        }
        std::optional<T> value{std::move(slots_[head & (N - 1)])};
        head_.store(head + 1, std::memory_order_release);
        return value;
    }

    static constexpr std::size_t capacity() noexcept { return N; }

private:
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // written by the consumer
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // written by the producer
    alignas(kCacheLine) std::array<T, N> slots_{};
};

// Bounded multi-producer, multi-consumer queue (D. Vyukov's algorithm).
// Capacity is set at construction and must be a power of two. Lock-free;
// a full queue rejects the push rather than waiting.
template <typename T>
class MpmcQueue {
public:
    explicit MpmcQueue(std::size_t capacity) : cells_(capacity), mask_(capacity - 1) {
        assert(capacity >= 2 && (capacity & (capacity - 1)) == 0);
        for (std::size_t i = 0; i < capacity; ++i) {
            cells_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    bool try_push(const T& value) {
        Cell* cell = nullptr;
        std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // full
            } else {
                pos = enqueue_pos_.load(std::memory_order_relaxed);
            }
        }
        cell->value = value;
        cell->sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        Cell* cell = nullptr;
        std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            cell = &cells_[pos & mask_];
            const std::size_t seq = cell->sequence.load(std::memory_order_acquire);
            const auto diff =
                static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
            if (diff == 0) {
                if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    break;
                }
            } else if (diff < 0) {
                return false;  // empty
            } else {
                pos = dequeue_pos_.load(std::memory_order_relaxed);
            }
        }
        out = std::move(cell->value);
        cell->sequence.store(pos + mask_ + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return mask_ + 1; }

private:
    struct Cell {
        std::atomic<std::size_t> sequence{0};
        T value{};
    };

    std::vector<Cell> cells_;
    std::size_t mask_;
    alignas(kCacheLine) std::atomic<std::size_t> enqueue_pos_{0};
    alignas(kCacheLine) std::atomic<std::size_t> dequeue_pos_{0};
};

// Wait-free handoff of the latest value from one writer to one reader
// (SceneState). The writer fills write_slot() and publishes it; the reader
// gets the most recent published value and never sees a partial one.
template <typename T>
class TripleBuffer {
public:
    // Writer side. The slot holds an older value: overwrite all of it.
    T& write_slot() noexcept { return slots_[back_]; }

    void publish() noexcept {
        back_ = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel) & kIndex;
    }

    // Reader side. Takes the newest published value if there is one; returns
    // the reader's current value either way.
    const T& read() noexcept {
        if ((middle_.load(std::memory_order_relaxed) & kDirty) != 0) {
            front_ = middle_.exchange(front_, std::memory_order_acq_rel) & kIndex;
        }
        return slots_[front_];
    }

    // True if a value was published since the reader last took one.
    bool has_new() const noexcept {
        return (middle_.load(std::memory_order_acquire) & kDirty) != 0;
    }

private:
    static constexpr unsigned kIndex = 0x3;
    static constexpr unsigned kDirty = 0x4;

    std::array<T, 3> slots_{};
    alignas(kCacheLine) unsigned back_ = 0;                // writer only
    alignas(kCacheLine) std::atomic<unsigned> middle_{1};  // shared
    alignas(kCacheLine) unsigned front_ = 2;               // reader only
};

}  // namespace fovea
