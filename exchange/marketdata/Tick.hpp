#pragma once
/**
 * @file exchange/marketdata/Tick.hpp
 * @brief Market data tick types: TradeTick, QuoteTick, OrderBookUpdate.
 *
 * Design
 * ──────
 * Three distinct tick types mirror real market data feed formats:
 *
 *  TradeTick      — a completed trade (price, qty, aggressor side)
 *                   Source: exchange trade prints, T&S feed
 *
 *  QuoteTick      — best bid/ask snapshot (NBBO or exchange BBO)
 *                   Source: exchange quote feed, consolidated tape
 *
 *  OrderBookUpdate — incremental order book delta (add/cancel/modify/trade)
 *                   Source: exchange full-depth feed (MDP3, ITCH, PITCH)
 *
 * All three share a common TickHeader for timestamp, symbol, and sequence
 * number.  Sequence numbers enable gap detection and out-of-order correction.
 *
 * CSV column layouts (used by TickReplayer)
 * ─────────────────────────────────────────
 *  TradeTick :  timestamp_ns,symbol,price,quantity,side,trade_id,exchange
 *  QuoteTick :  timestamp_ns,symbol,bid_px,bid_sz,ask_px,ask_sz,exchange
 *  BookUpdate:  timestamp_ns,symbol,action,side,price,quantity,order_id,level
 *
 * action values: A=add, C=cancel, M=modify, T=trade (level consumed)
 */

#include "core/Types.hpp"
#include <string>
#include <cstdint>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// Common header shared by all tick types
// ─────────────────────────────────────────────────────────────

struct TickHeader {
    Timestamp  timestamp{0};   ///< Nanosecond epoch (exchange time)
    Timestamp  recvTime{0};    ///< When tick was received/parsed (wall clock)
    Symbol     symbol;
    uint64_t   seqNo{0};       ///< Exchange sequence number
    std::string exchange;      ///< "NYSE", "NASDAQ", "CME", etc.

    /// Latency from exchange timestamp to receipt (ns). Negative = clock skew.
    [[nodiscard]] int64_t transportLatencyNs() const noexcept {
        return recvTime - timestamp;
    }
};

// ─────────────────────────────────────────────────────────────
// TradeTick — one completed trade (tape print)
// ─────────────────────────────────────────────────────────────

struct TradeTick {
    TickHeader header;
    Price      price{0.0};
    Quantity   quantity{0};
    Side       aggressorSide{Side::Buy};  ///< Which side initiated the trade
    uint64_t   tradeId{0};               ///< Exchange-assigned trade ID
    bool       isOTC{false};             ///< OTC/dark-pool print

    [[nodiscard]] double notional() const noexcept {
        return price * static_cast<double>(quantity);
    }
};

// ─────────────────────────────────────────────────────────────
// QuoteTick — best bid/ask snapshot
// ─────────────────────────────────────────────────────────────

struct QuoteTick {
    TickHeader header;
    Price      bidPrice{0.0};
    Quantity   bidSize{0};
    Price      askPrice{0.0};
    Quantity   askSize{0};

    [[nodiscard]] Price  midPrice() const noexcept {
        return (bidPrice > 0 && askPrice > 0)
                   ? (bidPrice + askPrice) / 2.0 : 0.0;
    }
    [[nodiscard]] Price  spread()   const noexcept {
        return (bidPrice > 0 && askPrice > 0)
                   ? askPrice - bidPrice : 0.0;
    }
    [[nodiscard]] bool   isCrossed()const noexcept {
        return bidPrice > 0 && askPrice > 0 && bidPrice >= askPrice;
    }
};

// ─────────────────────────────────────────────────────────────
// OrderBookUpdate — one incremental depth event
// ─────────────────────────────────────────────────────────────

enum class BookAction : uint8_t {
    Add    = 0,   ///< New order added to book
    Cancel = 1,   ///< Order cancelled / expired
    Modify = 2,   ///< Order modified (price or qty change)
    Trade  = 3,   ///< Level partially or fully consumed by trade
    Clear  = 4,   ///< Full book clear (e.g. after halt)
};

inline std::string bookActionName(BookAction a) {
    switch (a) {
        case BookAction::Add:    return "ADD";
        case BookAction::Cancel: return "CANCEL";
        case BookAction::Modify: return "MODIFY";
        case BookAction::Trade:  return "TRADE";
        case BookAction::Clear:  return "CLEAR";
    }
    return "?";
}

struct OrderBookUpdate {
    TickHeader header;
    BookAction action{BookAction::Add};
    Side       side{Side::Buy};
    Price      price{0.0};
    Quantity   quantity{0};
    OrderId    orderId{0};    ///< Exchange order reference number
    int        level{0};      ///< Price level depth (1=best)
};

// ─────────────────────────────────────────────────────────────
// TickType discriminator — for heterogeneous tick streams
// ─────────────────────────────────────────────────────────────

enum class TickType : uint8_t {
    Trade    = 0,
    Quote    = 1,
    BookUpdate = 2,
};

/**
 * @brief Tagged union wrapping any tick type.
 *
 * Used by the TickReplayer to return a single type from its iterator
 * regardless of which CSV column layout it parsed.
 */
struct AnyTick {
    TickType type{TickType::Quote};
    union {
        TradeTick    trade;
        QuoteTick    quote;
        OrderBookUpdate book;
    };

    AnyTick() : type{TickType::Quote}, quote{} {}
    ~AnyTick() { destroy(); }

    AnyTick(const AnyTick& o)            { copyFrom(o); }
    AnyTick(AnyTick&& o) noexcept        { copyFrom(o); }
    AnyTick& operator=(const AnyTick& o) { destroy(); copyFrom(o); return *this; }
    AnyTick& operator=(AnyTick&& o) noexcept { destroy(); copyFrom(o); return *this; }

    static AnyTick makeTrade(TradeTick t) {
        AnyTick a; a.destroy();
        a.type = TickType::Trade;
        new (&a.trade) TradeTick(std::move(t));
        return a;
    }
    static AnyTick makeQuote(QuoteTick q) {
        AnyTick a; a.destroy();
        a.type = TickType::Quote;
        new (&a.quote) QuoteTick(std::move(q));
        return a;
    }
    static AnyTick makeBook(OrderBookUpdate b) {
        AnyTick a; a.destroy();
        a.type = TickType::BookUpdate;
        new (&a.book) OrderBookUpdate(std::move(b));
        return a;
    }

    [[nodiscard]] Timestamp timestamp() const noexcept {
        switch (type) {
            case TickType::Trade:      return trade.header.timestamp;
            case TickType::Quote:      return quote.header.timestamp;
            case TickType::BookUpdate: return book.header.timestamp;
        }
        return 0;
    }
    [[nodiscard]] const Symbol& symbol() const noexcept {
        switch (type) {
            case TickType::Trade:      return trade.header.symbol;
            case TickType::Quote:      return quote.header.symbol;
            case TickType::BookUpdate: return book.header.symbol;
        }
        static Symbol empty;
        return empty;
    }

private:
    void destroy() noexcept {
        switch (type) {
            case TickType::Trade:      trade.~TradeTick();      break;
            case TickType::Quote:      quote.~QuoteTick();      break;
            case TickType::BookUpdate: book.~OrderBookUpdate(); break;
        }
    }
    void copyFrom(const AnyTick& o) {
        type = o.type;
        switch (type) {
            case TickType::Trade:      new (&trade) TradeTick(o.trade);          break;
            case TickType::Quote:      new (&quote) QuoteTick(o.quote);          break;
            case TickType::BookUpdate: new (&book)  OrderBookUpdate(o.book);     break;
        }
    }
};

} // namespace qtl
