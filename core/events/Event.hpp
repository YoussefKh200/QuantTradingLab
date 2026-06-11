#pragma once
/**
 * @file core/events/Event.hpp
 * @brief Base event class and all concrete event subtypes.
 *
 * Design decisions:
 *  - Polymorphic base with virtual destructor enables heterogeneous queuing
 *    via std::unique_ptr<Event>.
 *  - Each subtype is a plain-data struct; no heap allocations inside events
 *    (strings stored by value with SSO, numeric fields only).
 *  - EventType enum allows O(1) dispatch via switch rather than dynamic_cast.
 */

#include "core/Types.hpp"
#include <string>
#include <cstdint>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// Base Event
// ─────────────────────────────────────────────────────────────

struct Event {
    EventType  type;       ///< Discriminator for fast dispatch
    Timestamp  timestamp;  ///< Nanosecond epoch when event was created

    /// Used by EventPool<T> to store pool-pointer + slot-index for
    /// zero-allocation round-trip. Set to ~0 for heap-allocated events.
    /// Do NOT touch from user code.
    uintptr_t  poolContext_{~uintptr_t{0}};

    explicit Event(EventType t) noexcept
        : type{t}, timestamp{nowNs()} {}

    virtual ~Event() = default;

    // Non-copyable; moved through the queue.
    Event(const Event&)            = delete;
    Event& operator=(const Event&) = delete;
    Event(Event&&)                 = default;
    Event& operator=(Event&&)      = default;
};

// ─────────────────────────────────────────────────────────────
// MarketEvent — raw market data arriving from feed/replay
// ─────────────────────────────────────────────────────────────

struct MarketEvent final : Event {
    Symbol  symbol;
    Price   bidPrice;
    Price   askPrice;
    Price   lastPrice;
    Quantity bidSize;
    Quantity askSize;
    Quantity lastSize;

    MarketEvent(Symbol sym,
                Price bid, Quantity bidSz,
                Price ask, Quantity askSz,
                Price last, Quantity lastSz) noexcept
        : Event{EventType::Market}
        , symbol{std::move(sym)}
        , bidPrice{bid}, askPrice{ask}, lastPrice{last}
        , bidSize{bidSz}, askSize{askSz}, lastSize{lastSz}
    {}
};

// ─────────────────────────────────────────────────────────────
// OrderEvent — instruction from strategy to execution layer
// ─────────────────────────────────────────────────────────────

struct OrderEvent final : Event {
    OrderId    orderId;
    Symbol     symbol;
    OrderType  orderType;
    Side       side;
    Price      price;       ///< 0 for market orders
    Quantity   quantity;
    TimeInForce tif;
    std::string strategyId; ///< Tag for multi-strategy P&L attribution

    OrderEvent(OrderId id,
               Symbol sym,
               OrderType ot,
               Side s,
               Price p,
               Quantity q,
               TimeInForce t = TimeInForce::GTC,
               std::string sid = "") noexcept
        : Event{EventType::Order}
        , orderId{id}, symbol{std::move(sym)}
        , orderType{ot}, side{s}
        , price{p}, quantity{q}
        , tif{t}, strategyId{std::move(sid)}
    {}
};

// ─────────────────────────────────────────────────────────────
// FillEvent — execution report from matching engine
// ─────────────────────────────────────────────────────────────

struct FillEvent final : Event {
    OrderId  orderId;
    TradeId  tradeId;
    Symbol   symbol;
    Side     side;
    Price    fillPrice;
    Quantity fillQuantity;
    Quantity remainingQuantity;
    double   commission;     ///< Brokerage cost in base currency
    bool     isTaker;        ///< True if this fill was aggressive
    std::string strategyId;

    FillEvent(OrderId oid, TradeId tid,
              Symbol sym,
              Side s,
              Price fp, Quantity fq, Quantity rq,
              double comm, bool taker,
              std::string sid = "") noexcept
        : Event{EventType::Fill}
        , orderId{oid}, tradeId{tid}
        , symbol{std::move(sym)}, side{s}
        , fillPrice{fp}, fillQuantity{fq}, remainingQuantity{rq}
        , commission{comm}, isTaker{taker}
        , strategyId{std::move(sid)}
    {}
};

// ─────────────────────────────────────────────────────────────
// SignalEvent — directional signal from an alpha model
// ─────────────────────────────────────────────────────────────

struct SignalEvent final : Event {
    Symbol      symbol;
    double      strength;    ///< [-1.0, 1.0] normalised signal strength
    std::string source;      ///< Strategy / model that generated the signal

    SignalEvent(Symbol sym, double str, std::string src) noexcept
        : Event{EventType::Signal}
        , symbol{std::move(sym)}, strength{str}
        , source{std::move(src)}
    {}
};

// ─────────────────────────────────────────────────────────────
// RiskEvent — breach notification from the risk engine
// ─────────────────────────────────────────────────────────────

struct RiskEvent final : Event {
    enum class Severity : uint8_t { Warning, Breach, Kill };

    Symbol      symbol;     ///< Empty string = portfolio-level breach
    Severity    severity;
    std::string message;
    double      currentValue;
    double      limitValue;

    RiskEvent(Symbol sym, Severity sev,
              std::string msg,
              double current, double limit) noexcept
        : Event{EventType::Risk}
        , symbol{std::move(sym)}, severity{sev}
        , message{std::move(msg)}
        , currentValue{current}, limitValue{limit}
    {}
};

// ─────────────────────────────────────────────────────────────
// TimerEvent — scheduled callback for strategy timers
// ─────────────────────────────────────────────────────────────

struct TimerEvent final : Event {
    uint64_t    timerId;
    std::string label;

    TimerEvent(uint64_t id, std::string lbl) noexcept
        : Event{EventType::Timer}
        , timerId{id}, label{std::move(lbl)}
    {}
};

} // namespace qtl
