#pragma once
/**
 * @file exchange/orderbook/OrderBook.hpp
 * @brief Institutional-grade limit order book with FIFO price-time priority.
 *
 * Architecture
 * ────────────
 *   bids_    : map<Price, PriceLevel, greater<Price>>  — best bid = begin()
 *   asks_    : map<Price, PriceLevel, less<Price>>     — best ask = begin()
 *   orders_  : unordered_map<OrderId, Order>           — single ownership
 *   iterMap_ : unordered_map<OrderId, Iterator>        — O(1) cancel lookup
 *
 * Complexity
 * ──────────
 *   addOrder    O(log N)    N = distinct price levels
 *   cancelOrder O(log N)
 *   modifyOrder O(log N)    cancel + re-insert
 *   matchOrder  O(k log N)  k = levels consumed
 *   printBook   O(D)        D = requested depth
 */

#include "exchange/orderbook/Order.hpp"
#include "exchange/orderbook/PriceLevel.hpp"
#include <map>
#include <unordered_map>
#include <functional>
#include <vector>
#include <string>
#include <atomic>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// AddResult
// ─────────────────────────────────────────────────────────────

struct AddResult {
    bool                     accepted{false};
    bool                     immediatelyFilled{false};
    std::vector<TradeReport> trades;
};

// ─────────────────────────────────────────────────────────────
// OrderBook
// ─────────────────────────────────────────────────────────────

class OrderBook {
public:
    using TradeCallback = std::function<void(const TradeReport&)>;

    explicit OrderBook(Symbol symbol);
    ~OrderBook() = default;

    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    void setTradeCallback(TradeCallback cb) { onTrade_ = std::move(cb); }

    // ── Core API ─────────────────────────────────────────────
    [[nodiscard]] AddResult addOrder(Order order);
    bool                    cancelOrder(OrderId id);
    bool                    modifyOrder(const ModifyRequest& req);

    // ── Queries ───────────────────────────────────────────────
    [[nodiscard]] const Symbol& symbol()     const noexcept { return symbol_; }
    [[nodiscard]] Price         bestBid()    const noexcept;
    [[nodiscard]] Price         bestAsk()    const noexcept;
    [[nodiscard]] Price         midPrice()   const noexcept;
    [[nodiscard]] Price         spread()     const noexcept;
    [[nodiscard]] const Order*  findOrder(OrderId id) const noexcept;
    [[nodiscard]] Quantity      totalBidQty()const noexcept;
    [[nodiscard]] Quantity      totalAskQty()const noexcept;
    [[nodiscard]] size_t        bidLevels()  const noexcept { return bids_.size(); }
    [[nodiscard]] size_t        askLevels()  const noexcept { return asks_.size(); }
    [[nodiscard]] size_t        bidOrderCount() const noexcept { return bidOrderCount_; }
    [[nodiscard]] size_t        askOrderCount() const noexcept { return askOrderCount_; }
    [[nodiscard]] uint64_t      tradeCount() const noexcept { return tradeCount_; }
    [[nodiscard]] uint64_t      ordersAdded()const noexcept { return ordersAdded_; }

    struct DepthSnapshot {
        std::vector<std::pair<Price,Quantity>> bids;
        std::vector<std::pair<Price,Quantity>> asks;
    };
    [[nodiscard]] DepthSnapshot snapshot(size_t depth = 10) const;

    // ── Visualisation ─────────────────────────────────────────
    [[nodiscard]] std::string printBook(size_t depth = 10) const;

private:
    std::vector<TradeReport> matchAgainst(Order& aggressor);
    void       restOrder(Order& order);
    void       removeFromLevel(OrderId id);
    TradeReport executeTrade(Order& maker, Order& taker, Quantity qty);

    Symbol symbol_;
    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel, std::less<Price>>    asks_;
    std::unordered_map<OrderId, Order>               orders_;
    std::unordered_map<OrderId, PriceLevel::Iterator> iterMap_;

    uint64_t seqGen_{1};
    uint64_t tradeIdGen_{1};
    uint64_t tradeCount_{0};
    uint64_t ordersAdded_{0};
    size_t   bidOrderCount_{0};
    size_t   askOrderCount_{0};

    TradeCallback onTrade_;
};

} // namespace qtl
