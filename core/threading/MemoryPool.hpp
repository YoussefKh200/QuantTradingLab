#pragma once
/**
 * @file core/threading/MemoryPool.hpp
 * @brief Fixed-size slab memory pool for zero-allocation hot-path use.
 *
 * Problem
 * ───────
 * The global allocator (malloc/free) is:
 *   - Non-deterministic latency (OS call, heap lock, coalescing)
 *   - Cache-unfriendly (fragmentation causes cold cache lines)
 *   - Contended under multi-threaded load
 *
 * Solution: MemoryPool<T, Cap>
 * ──────────────────────────────
 * Pre-allocates Cap objects in a contiguous aligned slab.
 * Maintains a lock-free free-list via CAS on an atomic<uint32_t*>.
 * allocate() and deallocate() are O(1) with no system calls.
 *
 * Design
 * ──────
 * The slab stores Cap slots of sizeof(T) each, aligned to alignof(T).
 * The free-list is embedded into the unused storage of each free slot:
 *   - When free: slot[0..3] holds the index of the next free slot.
 *   - When live: slot contains the user object (placement-new).
 *
 * Thread safety
 * ─────────────
 * allocate(): single CAS on the free-list head (wait-free on success).
 * deallocate(): single CAS to push onto the free-list head.
 * Multiple threads can allocate/deallocate concurrently.
 *
 * ABA avoidance
 * ─────────────
 * We use a 64-bit tagged pointer (index in low 32 bits, version in high 32 bits)
 * to eliminate the ABA problem without requiring a double-wide CAS.
 *
 * Usage
 * ─────
 * @code
 *   MemoryPool<Order, 4096> pool;
 *
 *   // Hot path: O(1) allocation, no malloc
 *   Order* o = pool.allocate();
 *   new (o) Order{...};          // placement-new
 *   // ... use o ...
 *   o->~Order();                 // explicit destructor
 *   pool.deallocate(o);
 * @endcode
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cassert>
#include <array>
#include <stdexcept>
#include <type_traits>
#include <new>
#include <cstring>

namespace qtl {

template<typename T, size_t Cap>
class MemoryPool {
    static_assert(Cap > 0, "MemoryPool capacity must be > 0");
    static_assert(Cap <= (1u << 20), "MemoryPool cap must be <= 1M slots");
    // Each slot must be large enough to hold a next-index (uint32_t) when free
    static_assert(sizeof(T) >= sizeof(uint32_t),
                  "T must be at least 4 bytes for free-list embedding");

    static constexpr uint32_t kNull      = UINT32_MAX;
    static constexpr uint64_t kIndexMask = 0x0000'0000'FFFF'FFFFULL;
    static constexpr uint64_t kTagShift  = 32;

    // ── Storage ───────────────────────────────────────────────

    struct alignas(alignof(T)) Slot {
        std::byte storage[sizeof(T)];
    };

    alignas(64) std::array<Slot, Cap> slab_;   // contiguous slab
    std::atomic<uint64_t> head_{0};            // tagged: high32=version, low32=index
    std::atomic<size_t>   liveCount_{0};

public:
    MemoryPool() {
        // Build the initial free-list: 0 → 1 → 2 → … → Cap-1 → kNull
        for (uint32_t i = 0; i < static_cast<uint32_t>(Cap); ++i) {
            uint32_t next = (i + 1 < Cap) ? (i + 1) : kNull;
            // Store next-index in the first 4 bytes of slot storage
            std::memcpy(slab_[i].storage, &next, sizeof(uint32_t));
        }
        // Head points to slot 0, version = 0
        head_.store(makeTagged(0, 0), std::memory_order_release);
    }

    ~MemoryPool() {
        assert(liveCount_.load() == 0 &&
               "MemoryPool destroyed with live objects");
    }

    // Non-copyable, non-movable
    MemoryPool(const MemoryPool&)            = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    // ── Allocation ────────────────────────────────────────────

    /**
     * @brief Allocate a slot — O(1), no system call.
     * Returns a pointer to uninitialised storage; caller must placement-new.
     * @return Pointer to storage, or nullptr if pool is exhausted.
     */
    [[nodiscard]] T* allocate() noexcept {
        uint64_t old = head_.load(std::memory_order_acquire);
        while (true) {
            uint32_t idx = static_cast<uint32_t>(old & kIndexMask);
            if (idx == kNull) return nullptr;   // pool exhausted

            // Read next pointer from slot storage
            uint32_t nextIdx;
            std::memcpy(&nextIdx, slab_[idx].storage, sizeof(uint32_t));

            uint64_t neo = makeTagged(nextIdx,
                           static_cast<uint32_t>((old >> kTagShift) + 1));

            if (head_.compare_exchange_weak(old, neo,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                ++liveCount_;
                return reinterpret_cast<T*>(slab_[idx].storage);
            }
            // CAS failed → retry with updated 'old'
        }
    }

    /**
     * @brief Return a slot to the pool — O(1), no system call.
     * Caller must explicitly destroy the object before calling deallocate().
     */
    void deallocate(T* p) noexcept {
        if (!p) return;
        uint32_t idx = slotIndex(p);
        assert(idx < Cap && "deallocate: pointer not from this pool");

        uint64_t old = head_.load(std::memory_order_acquire);
        while (true) {
            uint32_t head = static_cast<uint32_t>(old & kIndexMask);
            // Embed the current head index into this slot's storage
            std::memcpy(slab_[idx].storage, &head, sizeof(uint32_t));

            uint64_t neo = makeTagged(idx,
                           static_cast<uint32_t>((old >> kTagShift) + 1));

            if (head_.compare_exchange_weak(old, neo,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                --liveCount_;
                return;
            }
        }
    }

    // ── Queries ───────────────────────────────────────────────

    [[nodiscard]] size_t liveCount()  const noexcept { return liveCount_.load(); }
    [[nodiscard]] size_t freeCount()  const noexcept { return Cap - liveCount_.load(); }
    static constexpr size_t capacity() noexcept { return Cap; }
    [[nodiscard]] bool full()          const noexcept { return liveCount_.load() == Cap; }
    [[nodiscard]] bool empty()         const noexcept { return liveCount_.load() == 0;  }

    /**
     * @brief Check whether a pointer was allocated from this pool.
     */
    [[nodiscard]] bool owns(const T* p) const noexcept {
        const std::byte* base = slab_[0].storage;
        const std::byte* ptr  = reinterpret_cast<const std::byte*>(p);
        return ptr >= base && ptr < base + Cap * sizeof(Slot);
    }

private:
    static uint64_t makeTagged(uint32_t idx, uint32_t version) noexcept {
        return (static_cast<uint64_t>(version) << kTagShift) |
               static_cast<uint64_t>(idx);
    }

    uint32_t slotIndex(const T* p) const noexcept {
        const std::byte* base = slab_[0].storage;
        const std::byte* ptr  = reinterpret_cast<const std::byte*>(p);
        ptrdiff_t offset = ptr - base;
        return static_cast<uint32_t>(offset / static_cast<ptrdiff_t>(sizeof(Slot)));
    }
};

// ─────────────────────────────────────────────────────────────
// ObjectPool<T,Cap> — like MemoryPool but manages construction
// ─────────────────────────────────────────────────────────────

/**
 * @class ObjectPool
 * @brief Like MemoryPool but constructs/destructs T automatically.
 *
 * acquire()  → placement-new with forwarded args, returns T*
 * release()  → calls ~T(), returns slot to pool
 *
 * Useful when T is a complex type (Order, ExecutionReport, etc.).
 */
template<typename T, size_t Cap>
class ObjectPool {
public:
    ObjectPool()  = default;
    ~ObjectPool() = default;

    ObjectPool(const ObjectPool&)            = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template<typename... Args>
    [[nodiscard]] T* acquire(Args&&... args) {
        T* slot = pool_.allocate();
        if (!slot) return nullptr;
        return new (slot) T(std::forward<Args>(args)...);
    }

    void release(T* p) noexcept {
        if (!p) return;
        p->~T();
        pool_.deallocate(p);
    }

    [[nodiscard]] size_t liveCount()  const noexcept { return pool_.liveCount(); }
    [[nodiscard]] size_t freeCount()  const noexcept { return pool_.freeCount(); }
    static constexpr size_t capacity() noexcept { return Cap; }

private:
    MemoryPool<T, Cap> pool_;
};

} // namespace qtl
