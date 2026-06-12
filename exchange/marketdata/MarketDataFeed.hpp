#pragma once
/**
 * @file exchange/marketdata/MarketDataFeed.hpp
 * @brief Market data feed — subscribes to tick streams and emits MarketEvents.
 *
 * Architecture
 * ────────────
 * The MarketDataFeed sits between raw tick data (from CSV files or a live
 * socket) and the EventLoop.  It:
 *
 *   1. Receives AnyTick objects (from TickReplayer or a live adapter).
 *   2. Converts them into MarketEvents and pushes onto the EventLoop.
 *   3. Maintains per-symbol state: last trade, last quote, OHLCV bars.
 *   4. Detects stale quotes, crossed markets, and sequence gaps.
 *
 * Subscriber model
 * ────────────────
 * Components register typed callbacks:
 *   onTrade(TradeTick)
 *   onQuote(QuoteTick)
 *   onBook(OrderBookUpdate)
 *
 * These fire synchronously before the tick is forwarded to the EventLoop,
 * allowing components like the MatchingEngine to process depth updates
 * before strategies see the MarketEvent.
 *
 * Bar aggregation
 * ───────────────
 * The feed tracks OHLCV bars at configurable intervals.  A BarEvent
 * is emitted when a bar closes.  Supported intervals: 1s, 5s, 1m, 5m.
 */

#include "exchange/marketdata/Tick.hpp"
#include "core/events/EventLoop.hpp"
#include "core/logger/Logger.hpp"

#include <unordered_map>
#include <functional>
#include <vector>
#include <string>
#include <memory>
#include <chrono>
#include <cstdint>
#include <limits>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// OHLCV Bar
// ─────────────────────────────────────────────────────────────

struct Bar {
    Symbol    symbol;
    Timestamp openTime{0};
    Timestamp closeTime{0};
    Price     open{0.0};
    Price     high{0.0};
    Price     low{std::numeric_limits<Price>::max()};
    Price     close{0.0};
    Quantity  volume{0};
    double    notional{0.0};    ///< Sum(price * qty) for VWAP
    uint64_t  tradeCount{0};

    [[nodiscard]] bool   isValid()  const noexcept { return tradeCount > 0; }
    [[nodiscard]] double vwap()     const noexcept {
        return volume > 0 ? notional / static_cast<double>(volume) : 0.0;
    }
    [[nodiscard]] Price  range()    const noexcept { return high - low; }

    void update(Price price, Quantity qty, Timestamp ts) noexcept {
        if (tradeCount == 0) { open = price; openTime = ts; }
        if (price > high)    high = price;
        if (price < low)     low  = price;
        close    = price;
        closeTime = ts;
        volume   += qty;
        notional += price * static_cast<double>(qty);
        ++tradeCount;
    }

    void reset(Timestamp ts) noexcept {
        openTime   = ts; closeTime = ts;
        open = high = low = close = 0.0;
        low = std::numeric_limits<Price>::max();
        volume = 0; notional = 0.0; tradeCount = 0;
    }
};

// ─────────────────────────────────────────────────────────────
// Per-symbol market state
// ─────────────────────────────────────────────────────────────

struct SymbolState {
    Symbol      symbol;
    TradeTick   lastTrade;
    QuoteTick   lastQuote;
    uint64_t    tradeCount{0};
    uint64_t    quoteCount{0};
    uint64_t    bookUpdateCount{0};
    uint64_t    seqGaps{0};
    uint64_t    lastSeqNo{0};

    // Running bar (1-minute default)
    Bar         currentBar;
    int64_t     barIntervalNs{60'000'000'000LL}; // 60 s

    void checkSeqGap(uint64_t seqNo) noexcept {
        if (lastSeqNo > 0 && seqNo > lastSeqNo + 1) ++seqGaps;
        lastSeqNo = seqNo;
    }
};

// ─────────────────────────────────────────────────────────────
// FeedStats — aggregate feed metrics
// ─────────────────────────────────────────────────────────────

struct FeedStats {
    uint64_t totalTicks{0};
    uint64_t tradeTicks{0};
    uint64_t quoteTicks{0};
    uint64_t bookTicks{0};
    uint64_t parseErrors{0};
    uint64_t seqGaps{0};
    uint64_t crossedQuotes{0};
    uint64_t barsEmitted{0};
    Timestamp firstTickTime{0};
    Timestamp lastTickTime{0};

    [[nodiscard]] double durationSec() const noexcept {
        return (lastTickTime > firstTickTime)
                   ? static_cast<double>(lastTickTime - firstTickTime) / 1e9
                   : 0.0;
    }
    [[nodiscard]] double ticksPerSec() const noexcept {
        double d = durationSec();
        return d > 0 ? static_cast<double>(totalTicks) / d : 0.0;
    }
};

// ─────────────────────────────────────────────────────────────
// MarketDataFeed
// ─────────────────────────────────────────────────────────────

class MarketDataFeed {
public:
    using TradeCallback    = std::function<void(const TradeTick&)>;
    using QuoteCallback    = std::function<void(const QuoteTick&)>;
    using BookCallback     = std::function<void(const OrderBookUpdate&)>;
    using BarCallback      = std::function<void(const Bar&)>;

    explicit MarketDataFeed(EventLoop* loop = nullptr)
        : eventLoop_{loop} {}

    // ── Subscriber registration ───────────────────────────────

    void onTrade(TradeCallback cb)  { tradeCb_  = std::move(cb); }
    void onQuote(QuoteCallback cb)  { quoteCb_  = std::move(cb); }
    void onBook(BookCallback  cb)   { bookCb_   = std::move(cb); }
    void onBar(BarCallback    cb)   { barCb_    = std::move(cb); }

    void setBarInterval(int64_t intervalNs) noexcept {
        barIntervalNs_ = intervalNs;
        for (auto& [sym, state] : states_) {
            state.barIntervalNs = intervalNs;
        }
    }

    // ── Tick ingestion — called by TickReplayer or live adapter ─

    /**
     * @brief Process one tick.
     *
     * Updates per-symbol state, fires typed callbacks, emits MarketEvent
     * onto EventLoop, checks for bar close.
     */
    void processTick(const AnyTick& tick) {
        ++stats_.totalTicks;

        Timestamp ts = tick.timestamp();
        if (stats_.firstTickTime == 0) stats_.firstTickTime = ts;
        stats_.lastTickTime = ts;

        const Symbol& sym = tick.symbol();
        auto& state = ensureSymbol(sym);

        switch (tick.type) {
            case TickType::Trade:
                processTrade(tick.trade, state);
                break;
            case TickType::Quote:
                processQuote(tick.quote, state);
                break;
            case TickType::BookUpdate:
                processBook(tick.book, state);
                break;
        }
    }

    // ── Queries ──────────────────────────────────────────────

    [[nodiscard]] const FeedStats& stats()   const noexcept { return stats_;  }
    [[nodiscard]] const SymbolState* getState(const Symbol& sym) const noexcept {
        auto it = states_.find(sym);
        return it == states_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] std::vector<Symbol> symbols() const {
        std::vector<Symbol> out;
        out.reserve(states_.size());
        for (auto& [k, _] : states_) out.push_back(k);
        return out;
    }

private:
    SymbolState& ensureSymbol(const Symbol& sym) {
        auto it = states_.find(sym);
        if (it == states_.end()) {
            auto [ins, _] = states_.emplace(sym, SymbolState{});
            ins->second.symbol          = sym;
            ins->second.barIntervalNs   = barIntervalNs_;
            return ins->second;
        }
        return it->second;
    }

    void processTrade(const TradeTick& t, SymbolState& state) {
        ++stats_.tradeTicks;
        ++state.tradeCount;
        state.checkSeqGap(t.header.seqNo);
        state.lastTrade = t;

        // Update running bar
        state.currentBar.update(t.price, t.quantity, t.header.timestamp);
        maybeCloseBar(state, t.header.timestamp);

        // Fire typed callback
        if (tradeCb_) tradeCb_(t);

        // Push MarketEvent onto EventLoop
        if (eventLoop_) {
            eventLoop_->emplace<MarketEvent>(
                t.header.symbol,
                state.lastQuote.bidPrice,
                state.lastQuote.bidSize,
                state.lastQuote.askPrice,
                state.lastQuote.askSize,
                t.price, t.quantity);
        }
    }

    void processQuote(const QuoteTick& q, SymbolState& state) {
        ++stats_.quoteTicks;
        ++state.quoteCount;
        state.checkSeqGap(q.header.seqNo);

        if (q.isCrossed()) ++stats_.crossedQuotes;
        state.lastQuote = q;

        if (quoteCb_) quoteCb_(q);

        if (eventLoop_) {
            eventLoop_->emplace<MarketEvent>(
                q.header.symbol,
                q.bidPrice, q.bidSize,
                q.askPrice, q.askSize,
                0.0, 0);  // no last trade
        }
    }

    void processBook(const OrderBookUpdate& u, SymbolState& state) {
        ++stats_.bookTicks;
        ++state.bookUpdateCount;
        state.checkSeqGap(u.header.seqNo);

        if (bookCb_) bookCb_(u);
        // Book updates do not generate a MarketEvent by default
        // (they're processed directly by the MatchingEngine)
    }

    void maybeCloseBar(SymbolState& state, Timestamp ts) {
        if (!state.currentBar.isValid()) return;
        if (ts - state.currentBar.openTime >= state.barIntervalNs) {
            state.currentBar.closeTime = ts;
            if (barCb_) barCb_(state.currentBar);
            ++stats_.barsEmitted;
            state.currentBar.reset(ts);
        }
    }

    std::unordered_map<Symbol, SymbolState>  states_;
    FeedStats                                stats_;
    EventLoop*                               eventLoop_{nullptr};
    int64_t                                  barIntervalNs_{60'000'000'000LL};

    TradeCallback  tradeCb_;
    QuoteCallback  quoteCb_;
    BookCallback   bookCb_;
    BarCallback    barCb_;
};

} // namespace qtl
