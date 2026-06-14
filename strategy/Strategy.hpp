#pragma once
/**
 * @file strategy/Strategy.hpp
 * @brief Abstract strategy base class and strategy execution context.
 *
 * Architecture
 * ────────────
 * Every strategy in QuantTradingLab implements IStrategy.  The framework
 * wires strategies into the EventLoop so they receive:
 *
 *   onMarket(MarketEvent)   — every tick (price/quote update)
 *   onFill(FillEvent)       — execution confirmation
 *   onRisk(RiskEvent)       — risk breach notification
 *   onTimer(TimerEvent)     — scheduled callbacks
 *   onStart()               — session open / backtest begin
 *   onStop()                — session close / backtest end
 *
 * StrategyContext
 * ───────────────
 * Every strategy receives a StrategyContext reference giving access to:
 *   - Order submission (submitOrder / cancelOrder / modifyOrder)
 *   - Current positions and P&L
 *   - Market state (best bid/ask, last trade)
 *   - Risk engine pre-trade check
 *   - Logger
 *
 * This decouples strategies from the concrete engine implementation —
 * the same strategy compiles against the live engine, the backtest engine,
 * and the paper trading engine.
 *
 * State machine
 * ─────────────
 *   Created → Initialised → Running → Stopping → Stopped
 *   Any state → Error (on unhandled exception in a handler)
 */

#include "core/Types.hpp"
#include "core/events/Event.hpp"
#include "core/logger/Logger.hpp"
#include "exchange/orderbook/Order.hpp"
#include "exchange/execution/ExecutionReport.hpp"

#include <string>
#include <memory>
#include <functional>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <stdexcept>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────
class IStrategy;
class StrategyContext;

// ─────────────────────────────────────────────────────────────
// StrategyState
// ─────────────────────────────────────────────────────────────

enum class StrategyState : uint8_t {
    Created,
    Initialised,
    Running,
    Stopping,
    Stopped,
    Error
};

inline std::string strategyStateName(StrategyState s) {
    switch (s) {
        case StrategyState::Created:     return "Created";
        case StrategyState::Initialised: return "Initialised";
        case StrategyState::Running:     return "Running";
        case StrategyState::Stopping:    return "Stopping";
        case StrategyState::Stopped:     return "Stopped";
        case StrategyState::Error:       return "Error";
    }
    return "Unknown";
}

// ─────────────────────────────────────────────────────────────
// StrategyStats — per-strategy performance counters
// ─────────────────────────────────────────────────────────────

struct StrategyStats {
    uint64_t ticksReceived{0};
    uint64_t ordersSubmitted{0};
    uint64_t ordersFilled{0};
    uint64_t ordersCancelled{0};
    uint64_t fillsReceived{0};
    double   realisedPnl{0.0};
    double   unrealisedPnl{0.0};
    double   totalCommission{0.0};
    Timestamp startTime{0};
    Timestamp lastTickTime{0};
};

// ─────────────────────────────────────────────────────────────
// StrategyContext — the strategy's window into the system
// ─────────────────────────────────────────────────────────────

class StrategyContext {
public:
    // Callbacks wired by the engine
    using SubmitFn  = std::function<std::vector<ExecutionReport>(Order)>;
    using CancelFn  = std::function<ExecutionReport(OrderId, const Symbol&)>;
    using ModifyFn  = std::function<ExecutionReport(const ModifyRequest&, const Symbol&)>;
    using PositionFn= std::function<Quantity(const Symbol&)>;
    using CashFn    = std::function<double()>;
    using BidFn     = std::function<Price(const Symbol&)>;
    using AskFn     = std::function<Price(const Symbol&)>;

    void bindSubmit  (SubmitFn   f) { submitFn_   = std::move(f); }
    void bindCancel  (CancelFn   f) { cancelFn_   = std::move(f); }
    void bindModify  (ModifyFn   f) { modifyFn_   = std::move(f); }
    void bindPosition(PositionFn f) { positionFn_ = std::move(f); }
    void bindCash    (CashFn     f) { cashFn_     = std::move(f); }
    void bindBid     (BidFn      f) { bidFn_      = std::move(f); }
    void bindAsk     (AskFn      f) { askFn_      = std::move(f); }

    // ── Order management ─────────────────────────────────────

    std::vector<ExecutionReport> submitOrder(Order order) {
        if (submitFn_) return submitFn_(std::move(order));
        return {};
    }

    ExecutionReport cancelOrder(OrderId id, const Symbol& sym) {
        if (cancelFn_) return cancelFn_(id, sym);
        return {};
    }

    ExecutionReport modifyOrder(const ModifyRequest& req, const Symbol& sym) {
        if (modifyFn_) return modifyFn_(req, sym);
        return {};
    }

    // ── Market state ─────────────────────────────────────────

    [[nodiscard]] Quantity position(const Symbol& sym) const {
        return positionFn_ ? positionFn_(sym) : 0;
    }
    [[nodiscard]] double cash() const {
        return cashFn_ ? cashFn_() : 0.0;
    }
    [[nodiscard]] Price bestBid(const Symbol& sym) const {
        return bidFn_ ? bidFn_(sym) : 0.0;
    }
    [[nodiscard]] Price bestAsk(const Symbol& sym) const {
        return askFn_ ? askFn_(sym) : 0.0;
    }
    [[nodiscard]] Price midPrice(const Symbol& sym) const {
        Price b = bestBid(sym), a = bestAsk(sym);
        return (b > 0 && a > 0) ? (b + a) / 2.0 : 0.0;
    }

    // ── Order ID generation ──────────────────────────────────

    [[nodiscard]] OrderId nextOrderId() noexcept {
        return nextId_++;
    }

    // ── Convenience order builders ───────────────────────────

    [[nodiscard]] Order makeLimitBuy(const Symbol& sym,
                                      Price price, Quantity qty,
                                      const std::string& stratId = "") const {
        Order o;
        o.id         = const_cast<StrategyContext*>(this)->nextId_++;
        o.symbol     = sym;
        o.side       = Side::Buy;
        o.type       = OrderType::Limit;
        o.price      = price;
        o.quantity   = qty;
        o.tif        = TimeInForce::GTC;
        o.strategyId = stratId;
        return o;
    }

    [[nodiscard]] Order makeLimitSell(const Symbol& sym,
                                       Price price, Quantity qty,
                                       const std::string& stratId = "") const {
        Order o;
        o.id         = const_cast<StrategyContext*>(this)->nextId_++;
        o.symbol     = sym;
        o.side       = Side::Sell;
        o.type       = OrderType::Limit;
        o.price      = price;
        o.quantity   = qty;
        o.tif        = TimeInForce::GTC;
        o.strategyId = stratId;
        return o;
    }

    [[nodiscard]] Order makeMarketBuy(const Symbol& sym, Quantity qty,
                                       const std::string& stratId = "") const {
        Order o;
        o.id         = const_cast<StrategyContext*>(this)->nextId_++;
        o.symbol     = sym;
        o.side       = Side::Buy;
        o.type       = OrderType::Market;
        o.quantity   = qty;
        o.strategyId = stratId;
        return o;
    }

    [[nodiscard]] Order makeMarketSell(const Symbol& sym, Quantity qty,
                                        const std::string& stratId = "") const {
        Order o;
        o.id         = const_cast<StrategyContext*>(this)->nextId_++;
        o.symbol     = sym;
        o.side       = Side::Sell;
        o.type       = OrderType::Market;
        o.quantity   = qty;
        o.strategyId = stratId;
        return o;
    }

private:
    SubmitFn   submitFn_;
    CancelFn   cancelFn_;
    ModifyFn   modifyFn_;
    PositionFn positionFn_;
    CashFn     cashFn_;
    BidFn      bidFn_;
    AskFn      askFn_;
    OrderId nextId_{1000000};
};

// ─────────────────────────────────────────────────────────────
// IStrategy — abstract base
// ─────────────────────────────────────────────────────────────

class IStrategy {
public:
    explicit IStrategy(std::string name, std::string symbol = "")
        : name_{std::move(name)}, symbol_{std::move(symbol)} {}

    virtual ~IStrategy() = default;

    // Non-copyable
    IStrategy(const IStrategy&)            = delete;
    IStrategy& operator=(const IStrategy&) = delete;

    // ── Lifecycle ────────────────────────────────────────────

    /** Called once before any market data arrives. */
    virtual void onStart(StrategyContext& ctx) {
        state_ = StrategyState::Running;
        stats_.startTime = nowNs();
        Logger::instance().info(name_,
            "Strategy started (symbol={})", symbol_);
    }

    /** Called once after the last market event. */
    virtual void onStop(StrategyContext& ctx) {
        state_ = StrategyState::Stopped;
        Logger::instance().info(name_,
            "Strategy stopped. fills={} realisedPnl={:.2f}",
            stats_.fillsReceived, stats_.realisedPnl);
    }

    // ── Event handlers (override in concrete strategies) ─────

    virtual void onMarket(const MarketEvent& e,  StrategyContext& ctx) {}
    virtual void onFill  (const FillEvent&   e,  StrategyContext& ctx) {}
    virtual void onRisk  (const RiskEvent&   e,  StrategyContext& ctx) {
        if (e.severity == RiskEvent::Severity::Kill) {
            state_ = StrategyState::Stopping;
        }
    }
    virtual void onTimer (const TimerEvent&  e,  StrategyContext& ctx) {}

    // ── Accessors ────────────────────────────────────────────

    [[nodiscard]] const std::string&  name()   const noexcept { return name_;   }
    [[nodiscard]] const std::string&  symbol() const noexcept { return symbol_; }
    [[nodiscard]] StrategyState       state()  const noexcept { return state_;  }
    [[nodiscard]] const StrategyStats& stats() const noexcept { return stats_;  }
    [[nodiscard]] bool isRunning() const noexcept {
        return state_ == StrategyState::Running;
    }

protected:
    std::string    name_;
    std::string    symbol_;
    StrategyState  state_{StrategyState::Created};
    StrategyStats  stats_;

    // ── P&L tracking helpers for subclasses ──────────────────

    void recordFill(Side side, Price price, Quantity qty, double commission) {
        ++stats_.fillsReceived;
        double notional = price * static_cast<double>(qty);
        double pnl = (side == Side::Sell ? +1.0 : -1.0) * notional - commission;
        stats_.realisedPnl  += pnl;
        stats_.totalCommission += commission;
    }

    void recordOrderSubmitted() { ++stats_.ordersSubmitted; }
    void recordTick()           { ++stats_.ticksReceived;   }
};

} // namespace qtl
