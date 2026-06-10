#pragma once
/**
 * @file core/threading/RingBuffer.hpp
 * @brief Lock-free Single-Producer / Single-Consumer (SPSC) ring buffer.
 *
 * Used on the hot path: market-data feed thread → strategy thread.
 *
 * Properties:
 *  - Wait-free: both push and pop are O(1) with no CAS retry loops.
 *  - Cache-line padding isolates head/tail counters to avoid false sharing.
 *  - Capacity must be a power of two (asserted at construction).
 *  - Stores T by value; T must be move-constructible.
 *
 * Template parameter N: capacity (must be power of 2, e.g. 1<<16 = 65536).
 */

#include <atomic>
#include <array>
#include <optional>
#include <stdexcept>
#include <cstdint>

namespace qtl {

template<typename T, size_t N>
class SPSCRingBuffer {
    static_assert((N & (N - 1)) == 0, "Capacity N must be a power of two");
    static_assert(N >= 2, "Capacity N must be at least 2");

public:
    SPSCRingBuffer() = default;

    // ─── Producer API (call from one thread only) ──────────────

    /**
     * @brief Try to push an item.
     * @return true if pushed; false if the buffer is full (caller must retry).
     */
    [[nodiscard]] bool tryPush(T item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t nextHead = (head + 1) & kMask;
        if (nextHead == tail_.load(std::memory_order_acquire)) {
            return false;  // full
        }
        buffer_[head] = std::move(item);
        head_.store(nextHead, std::memory_order_release);
        return true;
    }

    // ─── Consumer API (call from one thread only) ──────────────

    /**
     * @brief Try to pop an item.
     * @return The item, or std::nullopt if the buffer is empty.
     */
    [[nodiscard]] std::optional<T> tryPop() noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return std::nullopt;  // empty
        }
        T item = std::move(buffer_[tail]);
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return item;
    }

    // ─── State ─────────────────────────────────────────────────

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool full() const noexcept {
        return ((head_.load(std::memory_order_acquire) + 1) & kMask) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t size() const noexcept {
        return (head_.load(std::memory_order_acquire) -
                tail_.load(std::memory_order_acquire)) & kMask;
    }

    static constexpr size_t capacity() noexcept { return N; }

private:
    static constexpr size_t kMask = N - 1;

    // Separate head and tail onto different cache lines to eliminate
    // false sharing between producer and consumer.
    // 64 bytes is correct for x86-64/ARM64; avoids -Winterference-size noise.
    static constexpr size_t kCacheLineSize = 64;

    alignas(kCacheLineSize) std::atomic<size_t> head_{0};
    alignas(kCacheLineSize) std::atomic<size_t> tail_{0};

    std::array<T, N> buffer_{};
};

} // namespace qtl
