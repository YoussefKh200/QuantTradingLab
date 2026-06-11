#pragma once
/**
 * @file core/events/EventPool.hpp
 * @brief Lock-free, fixed-size object pool for hot-path event allocation.
 *
 * Problem
 * ───────
 * std::make_unique<MarketEvent>() calls the global allocator on every tick.
 * At 1 M ticks/sec that is 1 M malloc/free pairs — heap contention, cache
 * thrash, and latency spikes.
 *
 * Solution
 * ────────
 * Pre-allocate a slab of Cap aligned slots.  Hand out slots via an atomic
 * free-list (one CAS per alloc/free).  The unique_ptr deleter captures the
 * pool pointer + slot index directly, returning the slot on destruction.
 *
 * Properties
 * ──────────
 *  • O(1) alloc and free (single CAS on the uncontended fast path)
 *  • Falls back to heap when exhausted — never blocks or throws
 *  • Thread-safe multi-producer alloc; single-slot CAS free
 *  • No extra heap allocation for the deleter (lambda capture stored inline
 *    via std::unique_ptr<T, Deleter> with a stateful deleter struct)
 *
 * Usage
 * ─────
 * @code
 *   EventPool<MarketEvent, 4096> pool;          // once at startup
 *   auto ev = pool.make("AAPL", 182.5, 200, …); // hot path, zero malloc
 *   queue.push(std::move(ev));                  // returned to pool on pop
 * @endcode
 */

#include "core/events/Event.hpp"
#include <array>
#include <atomic>
#include <memory>
#include <cassert>
#include <cstddef>
#include <type_traits>

namespace qtl {

template<typename T, size_t Cap>
class EventPool {
    static_assert(std::is_base_of_v<Event, T>, "T must derive from Event");
    static_assert(Cap >= 2 && (Cap & (Cap - 1)) == 0,
                  "Cap must be a power-of-two >= 2");

    static constexpr uint32_t kNull = UINT32_MAX;

    // ── Aligned storage ───────────────────────────────────────────
    struct alignas(alignof(T)) Slot { std::byte storage[sizeof(T)]; };
    std::array<Slot, Cap>                 slots_;
    std::array<std::atomic<uint32_t>, Cap> nextFree_;
    std::atomic<uint32_t>                 head_{0};
    std::atomic<size_t>                   live_{0};

public:
    EventPool() {
        for (uint32_t i = 0; i < static_cast<uint32_t>(Cap) - 1; ++i)
            nextFree_[i].store(i + 1, std::memory_order_relaxed);
        nextFree_[Cap - 1].store(kNull, std::memory_order_relaxed);
        head_.store(0, std::memory_order_release);
    }

    ~EventPool() {
        assert(live_.load() == 0 && "EventPool destroyed with live events");
    }

    // ── Stateful deleter (stored inside unique_ptr, no heap) ──────

    struct Deleter {
        EventPool* pool{nullptr};
        uint32_t   idx{kNull};
        bool       fromPool{false};

        void operator()(T* obj) const noexcept {
            if (!fromPool) {
                delete obj;
                return;
            }
            obj->~T();
            pool->releaseSlot(idx);
        }
    };

    using PoolPtr = std::unique_ptr<T, Deleter>;

    // ── make() — primary allocation API ──────────────────────────

    template<typename... Args>
    [[nodiscard]] PoolPtr make(Args&&... args) {
        uint32_t idx = acquireSlot();
        if (idx != kNull) {
            T* obj = new (&slots_[idx].storage) T(std::forward<Args>(args)...);
            ++live_;
            return PoolPtr{obj, Deleter{this, idx, true}};
        }
        // Pool exhausted — heap fallback
        T* obj = new T(std::forward<Args>(args)...);
        return PoolPtr{obj, Deleter{nullptr, kNull, false}};
    }

    // ── Statistics ────────────────────────────────────────────────
    [[nodiscard]] size_t liveCount() const noexcept { return live_.load(); }
    [[nodiscard]] size_t freeCount() const noexcept { return Cap - live_.load(); }
    static constexpr size_t capacity() noexcept { return Cap; }

private:
    [[nodiscard]] uint32_t acquireSlot() noexcept {
        uint32_t h = head_.load(std::memory_order_acquire);
        while (h != kNull) {
            uint32_t nx = nextFree_[h].load(std::memory_order_relaxed);
            if (head_.compare_exchange_weak(h, nx,
                    std::memory_order_acq_rel, std::memory_order_acquire))
                return h;
        }
        return kNull;
    }

    void releaseSlot(uint32_t idx) noexcept {
        uint32_t h = head_.load(std::memory_order_acquire);
        do {
            nextFree_[idx].store(h, std::memory_order_relaxed);
        } while (!head_.compare_exchange_weak(h, idx,
                     std::memory_order_acq_rel, std::memory_order_acquire));
        --live_;
    }
};

// ─────────────────────────────────────────────────────────────
// Convenience alias: a unique_ptr<Event> that may or may not
// come from a pool.  The queue stores this type so it is
// agnostic about allocation strategy.
// ─────────────────────────────────────────────────────────────

/// Type-erased pool-aware event pointer.
/// For pool events the deleter returns the slot; for heap events it deletes.
/// Use make_heap_event<T>(...) when a pool is not available.
template<typename T, typename... Args>
[[nodiscard]] inline std::unique_ptr<Event>
make_heap_event(Args&&... args) {
    return std::make_unique<T>(std::forward<Args>(args)...);
}

} // namespace qtl
