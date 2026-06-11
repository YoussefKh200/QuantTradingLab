#pragma once
/**
 * @file exchange/orderbook/PriceLevel.hpp
 * @brief One price level in the limit order book.
 *
 * A PriceLevel holds all resting limit orders at a single price point.
 * Orders within a level are served in FIFO order (by seqNo).
 *
 * Data structure choice: std::list<Order*>
 *   • O(1) insert at back
 *   • O(1) remove from any position via stored iterator
 *   • Stable iterators — cancelling one order does not invalidate others
 *   • Orders themselves live in the OrderBook's flat map (no extra heap)
 *
 * The level does NOT own orders; it holds non-owning pointers.
 * The OrderBook is the single owner (via unordered_map<OrderId, Order>).
 */

#include "exchange/orderbook/Order.hpp"
#include <list>
#include <cstddef>

namespace qtl {

class PriceLevel {
public:
    using Queue    = std::list<Order*>;
    using Iterator = Queue::iterator;

    explicit PriceLevel(Price price) noexcept : price_{price} {}

    // ── Mutation ─────────────────────────────────────────────

    /// Append order to the back of the FIFO queue.
    Iterator add(Order* order) {
        totalQty_ += order->remainingQty();
        return queue_.insert(queue_.end(), order);
    }

    /// Remove the order pointed to by @p it.  Must be called before the
    /// order is cancelled in the book's master map.
    void remove(Iterator it) {
        totalQty_ -= (*it)->remainingQty();
        queue_.erase(it);
    }

    /// Reduce the level's cached total by @p qty (called after a partial fill).
    void reduceQty(Quantity qty) noexcept {
        totalQty_ -= qty;
        if (totalQty_ < 0) totalQty_ = 0;
    }

    // ── Queries ──────────────────────────────────────────────

    [[nodiscard]] Price    price()    const noexcept { return price_;           }
    [[nodiscard]] Quantity totalQty() const noexcept { return totalQty_;        }
    [[nodiscard]] size_t   numOrders()const noexcept { return queue_.size();    }
    [[nodiscard]] bool     empty()    const noexcept { return queue_.empty();   }

    /// Access the front-of-queue (oldest) order.
    [[nodiscard]] Order* front() const noexcept {
        return queue_.empty() ? nullptr : queue_.front();
    }

    // ── Iteration (for matching engine) ──────────────────────
    [[nodiscard]] Queue&       orders()       noexcept { return queue_; }
    [[nodiscard]] const Queue& orders() const noexcept { return queue_; }

private:
    Price    price_;
    Quantity totalQty_{0};
    Queue    queue_;
};

} // namespace qtl
