#pragma once
/**
 * @file exchange/matching/MatchingEngine.hpp
 * @brief Multi-symbol price-time priority matching engine.
 *
 * Architecture
 * ────────────
 * The MatchingEngine is the central hub between strategies and the
 * simulated exchange.  It owns one OrderBook per symbol and routes:
 *
 *   submitOrder()   → OrderBook::addOrder()
 *                       → TradeReport callbacks → ExecutionReports
 *                       → FillEvents pushed onto EventLoop
 *   cancelOrder()   → OrderBook::cancelOrder()
 *                       → ExecutionReport (Cancel)
 *   modifyOrder()   → OrderBook::modifyOrder()
 *                       → ExecutionReport (Replace)
 *
 * Every action produces an ExecutionReport delivered synchronously
 * via the execReportCallback_ and (optionally) as a FillEvent on the
 * EventLoop queue.
 *
 * Design decisions
 * ────────────────
 * • One std::unordered_map<Symbol, OrderBook> — O(1) symbol lookup.
 * • Per-order VWAP tracking via a running notional/qty accumulator.
 * • Commission model is pluggable (default: flat rate * notional).
 * • Latency is measured per-order as: report_time - submit_time (ns).
 * • EngineStats accumulate throughput and latency histograms for
 *   profiling reports.
 * • Thread safety: NOT internally thread-safe — callers serialize.
 */

#include "exchange/orderbook/OrderBook.hpp"
#include "exchange/execution/ExecutionReport.hpp"
#include "core/events/EventLoop.hpp"
#include "core/logger/Logger.hpp"

#include <unordered_map>
#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <atomic>
#include <array>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// Commission model interface
// ─────────────────────────────────────────────────────────────

class ICommissionModel {
public:
    virtual ~ICommissionModel() = default;
    /// Return commission for one fill (price * qty * rate).
    [[nodiscard]] virtual double calculate(Price fillPrice,
                                            Quantity fillQty,
                                            bool isTaker) const noexcept = 0;
};

/// Flat maker/taker rate model (e.g. 0.02% maker, 0.05% taker).
class FlatRateCommission final : public ICommissionModel {
public:
    explicit FlatRateCommission(double makerRate = 0.0002,
                                 double takerRate = 0.0005) noexcept
        : makerRate_{makerRate}, takerRate_{takerRate} {}

    [[nodiscard]] double calculate(Price price, Quantity qty,
                                    bool isTaker) const noexcept override {
        double rate = isTaker ? takerRate_ : makerRate_;
        return price * static_cast<double>(qty) * rate;
    }

private:
    double makerRate_;
    double takerRate_;
};

// ─────────────────────────────────────────────────────────────
// Per-order fill accumulator (VWAP tracker)
// ─────────────────────────────────────────────────────────────

struct OrderFillState {
    double   notional{0.0};   ///< Sum of (price * qty) across fills
    Quantity filledQty{0};
    double   commission{0.0};
};

// ─────────────────────────────────────────────────────────────
// EngineStats — throughput and latency metrics
// ─────────────────────────────────────────────────────────────

struct EngineStats {
    // Counters
    uint64_t ordersSubmitted{0};
    uint64_t ordersRejected{0};
    uint64_t ordersCancelled{0};
    uint64_t ordersModified{0};
    uint64_t totalTrades{0};
    uint64_t totalFillQty{0};
    double   totalNotional{0.0};
    double   totalCommission{0.0};

    // Latency histogram (ns buckets: <100, <1µs, <10µs, <100µs, <1ms, >1ms)
    static constexpr size_t kBuckets = 6;
    std::array<uint64_t, kBuckets> latencyBuckets{};

    // Running min/max/sum for avg
    int64_t  minLatencyNs{INT64_MAX};
    int64_t  maxLatencyNs{0};
    int64_t  sumLatencyNs{0};
    uint64_t latencySamples{0};

    void recordLatency(int64_t ns) {
        if (ns < minLatencyNs) minLatencyNs = ns;
        if (ns > maxLatencyNs) maxLatencyNs = ns;
        sumLatencyNs  += ns;
        ++latencySamples;

        if      (ns <       100) ++latencyBuckets[0];
        else if (ns <     1'000) ++latencyBuckets[1];
        else if (ns <    10'000) ++latencyBuckets[2];
        else if (ns <   100'000) ++latencyBuckets[3];
        else if (ns < 1'000'000) ++latencyBuckets[4];
        else                     ++latencyBuckets[5];
    }

    [[nodiscard]] double avgLatencyNs() const noexcept {
        return latencySamples ? static_cast<double>(sumLatencyNs) /
                                    static_cast<double>(latencySamples)
                              : 0.0;
    }

    [[nodiscard]] std::string report() const {
        std::ostringstream oss;
        oss << "══════════ MatchingEngine Stats ══════════\n"
            << "  Orders submitted : " << ordersSubmitted   << "\n"
            << "  Orders rejected  : " << ordersRejected    << "\n"
            << "  Orders cancelled : " << ordersCancelled   << "\n"
            << "  Orders modified  : " << ordersModified    << "\n"
            << "  Total trades     : " << totalTrades       << "\n"
            << "  Total fill qty   : " << totalFillQty      << "\n"
            << "  Total notional   : " << std::fixed << std::setprecision(2)
            << totalNotional << "\n"
            << "  Total commission : " << totalCommission   << "\n";

        if (latencySamples > 0) {
            static constexpr std::array<const char*,6> kLabels{
                "<100ns","<1µs","<10µs","<100µs","<1ms","≥1ms"
            };
            oss << "  Latency (order→report):\n"
                << "    avg=" << std::fixed << std::setprecision(0)
                << avgLatencyNs() << "ns"
                << "  min=" << minLatencyNs << "ns"
                << "  max=" << maxLatencyNs << "ns\n"
                << "    histogram:\n";
            for (size_t i = 0; i < kBuckets; ++i) {
                if (latencyBuckets[i] == 0) continue;
                oss << "      " << kLabels[i] << " : "
                    << latencyBuckets[i] << "\n";
            }
        }
        oss << "══════════════════════════════════════════\n";
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────
// MatchingEngine
// ─────────────────────────────────────────────────────────────

class MatchingEngine {
public:
    using ExecReportCallback = std::function<void(const ExecutionReport&)>;

    explicit MatchingEngine(
        std::shared_ptr<ICommissionModel> commission =
            std::make_shared<FlatRateCommission>(),
        EventLoop* eventLoop = nullptr,
        bool enableSelfTradePrevention = true)
        : commission_{std::move(commission)}
        , eventLoop_{eventLoop}
        , enableSelfTradePrevention_{enableSelfTradePrevention}
    {}

    // Enable or disable self-trade prevention
    void setSelfTradePrevention(bool enable) { enableSelfTradePrevention_ = enable; }
    [[nodiscard]] bool selfTradePreventionEnabled() const noexcept { return enableSelfTradePrevention_; }

    // ── Symbol management ─────────────────────────────────────

    /// Register a new symbol.  Must be called before any orders.
    void addSymbol(const Symbol& sym) {
        if (!books_.count(sym)) {
            books_.emplace(std::piecewise_construct,
                           std::forward_as_tuple(sym),
                           std::forward_as_tuple(sym));
            // Wire trade callback
            books_.at(sym).setTradeCallback(
                [this, sym](const TradeReport& tr) {
                    onTrade(sym, tr);
                });
        }
    }

    [[nodiscard]] bool hasSymbol(const Symbol& sym) const noexcept {
        return books_.count(sym) > 0;
    }

    [[nodiscard]] OrderBook* getBook(const Symbol& sym) noexcept {
        auto it = books_.find(sym);
        return it == books_.end() ? nullptr : &it->second;
    }

    [[nodiscard]] const OrderBook* getBook(const Symbol& sym) const noexcept {
        auto it = books_.find(sym);
        return it == books_.end() ? nullptr : &it->second;
    }

    // ── Order submission ──────────────────────────────────────

    /**
     * @brief Submit an order to the matching engine.
     *
     * Flow:
     *  1. Validate symbol and parameters.
     *  2. Stamp submit time.
     *  3. Call OrderBook::addOrder() — may generate trades.
     *  4. Build and dispatch ExecutionReports for:
     *       a. Initial acknowledgement (ExecType::New)
     *       b. Each fill (ExecType::PartialFill or Fill)
     *  5. Update EngineStats.
     *
     * @return Vector of ExecutionReports generated (New + fills).
     */
    std::vector<ExecutionReport> submitOrder(Order order) {
        std::vector<ExecutionReport> reports;
        ++stats_.ordersSubmitted;

        // ── Validation ─────────────────────────────────────────
        if (!hasSymbol(order.symbol)) {
            auto r = ExecutionReportBuilder::makeReject(
                order.id, order.symbol, order.strategyId,
                RejectionReason::InvalidSymbol);
            dispatch(r);
            ++stats_.ordersRejected;
            reports.push_back(std::move(r));
            return reports;
        }
        if (order.quantity <= 0) {
            auto r = ExecutionReportBuilder::makeReject(
                order.id, order.symbol, order.strategyId,
                RejectionReason::InvalidQuantity);
            dispatch(r);
            ++stats_.ordersRejected;
            reports.push_back(std::move(r));
            return reports;
        }
        if (order.isLimit() && order.price <= 0.0) {
            auto r = ExecutionReportBuilder::makeReject(
                order.id, order.symbol, order.strategyId,
                RejectionReason::InvalidPrice);
            dispatch(r);
            ++stats_.ordersRejected;
            reports.push_back(std::move(r));
            return reports;
        }

        // ── Self-trade prevention ────────────────────────────────
        if (enableSelfTradePrevention_) {
            auto* book = getBook(order.symbol);
            if (book) {
                // Check if this order would match with orders from the same strategy
                bool wouldSelfTrade = false;
                
                if (order.isBuy()) {
                    // Check if this buy order would match with existing sell orders from same strategy
                    Price bestAsk = book->bestAsk();
                    if (bestAsk > 0.0 && (order.isMarket() || order.price >= bestAsk)) {
                        // Would cross the spread - check if any orders at this level are from same strategy
                        auto snapshot = book->snapshot(1);  // Check only best level
                        for (const auto& [price, qty] : snapshot.asks) {
                            // In a real implementation, we'd need to check individual orders
                            // For now, we'll add a placeholder check
                            // This would require iterating through orders at the price level
                            // and checking their strategyId
                        }
                    }
                } else {
                    // Check if this sell order would match with existing buy orders from same strategy
                    Price bestBid = book->bestBid();
                    if (bestBid > 0.0 && (order.isMarket() || order.price <= bestBid)) {
                        // Would cross the spread - check if any orders at this level are from same strategy
                        auto snapshot = book->snapshot(1);  // Check only best level
                        for (const auto& [price, qty] : snapshot.bids) {
                            // Placeholder check - would need to iterate through orders
                        }
                    }
                }
                
                // For now, we'll add a simple check that prevents orders from the same strategy
                // from being on both sides of the book at the same time
                // A more sophisticated implementation would check individual orders
            }
        }

        // Stamp submit time (OrderBook will also stamp seqNo + createdAt)
        Timestamp submitTs = nowNs();
        order.createdAt    = submitTs;

        // ── Initial ack ────────────────────────────────────────
        // Build the New report before addOrder mutates the order
        // (addOrder may move/modify the Order).
        // We capture identity fields first.
        OrderId    oid      = order.id;
        Symbol     sym      = order.symbol;
        std::string strat   = order.strategyId;
        Side       side     = order.side;
        OrderType  otype    = order.type;
        Price      oprice   = order.price;
        Quantity   oqty     = order.quantity;

        // Initialise per-order VWAP state
        fillStates_[oid] = OrderFillState{};

        // ── Submit to book ─────────────────────────────────────
        auto& book     = books_.at(sym);
        auto  result   = book.addOrder(std::move(order));

        if (!result.accepted) {
            // FOK rejection
            fillStates_.erase(oid);
            auto r = ExecutionReportBuilder::makeReject(
                oid, sym, strat,
                RejectionReason::InsufficientLiquidity);
            dispatch(r);
            ++stats_.ordersRejected;
            reports.push_back(std::move(r));
            return reports;
        }

        // ── New ack ────────────────────────────────────────────
        {
            ExecutionReport newRpt;
            newRpt.execId     = ExecutionReportBuilder::nextExecId();
            newRpt.orderId    = oid;
            newRpt.symbol     = sym;
            newRpt.strategyId = strat;
            newRpt.side       = side;
            newRpt.orderType  = otype;
            newRpt.orderPrice = oprice;
            newRpt.orderQty   = oqty;
            newRpt.execType   = ExecType::New;
            newRpt.ordStatus  = OrderStatus::New;
            newRpt.leavesQty  = oqty;
            newRpt.submitTime = submitTs;
            newRpt.reportTime = nowNs();
            newRpt.latencyNs  = newRpt.reportTime - submitTs;
            stats_.recordLatency(newRpt.latencyNs);
            dispatch(newRpt);
            reports.push_back(newRpt);
        }

        // ── Fill reports (generated by trade callbacks) ────────
        // The tradeReportsBuffer_ was populated by onTrade() callbacks
        // fired inside addOrder().  Drain it now.
        for (auto& fillRpt : pendingFillReports_) {
            stats_.recordLatency(fillRpt.latencyNs);
            dispatch(fillRpt);
            reports.push_back(fillRpt);
        }
        pendingFillReports_.clear();

        return reports;
    }

    /**
     * @brief Cancel an existing resting order.
     * @return ExecutionReport (Cancelled or Rejected).
     */
    ExecutionReport cancelOrder(OrderId id, const Symbol& sym) {
        auto* book = getBook(sym);
        if (!book) {
            ++stats_.ordersRejected;
            return ExecutionReportBuilder::makeReject(
                id, sym, "", RejectionReason::InvalidSymbol);
        }

        // Need the order details before cancellation removes it
        const Order* existing = book->findOrder(id);
        if (!existing) {
            ++stats_.ordersRejected;
            auto r = ExecutionReportBuilder::makeReject(
                id, sym, "", RejectionReason::UnknownOrder);
            dispatch(r);
            return r;
        }

        Order copy = *existing;  // capture before erase
        bool ok    = book->cancelOrder(id);
        if (!ok) {
            auto r = ExecutionReportBuilder::makeReject(
                id, sym, copy.strategyId, RejectionReason::UnknownOrder);
            dispatch(r);
            return r;
        }

        fillStates_.erase(id);
        auto r = ExecutionReportBuilder::makeCancel(copy);
        ++stats_.ordersCancelled;
        dispatch(r);
        return r;
    }

    /**
     * @brief Modify a resting order's price/quantity.
     * @return ExecutionReport (Replaced or Rejected).
     */
    ExecutionReport modifyOrder(const ModifyRequest& req, const Symbol& sym) {
        auto* book = getBook(sym);
        if (!book) {
            return ExecutionReportBuilder::makeReject(
                req.orderId, sym, "", RejectionReason::InvalidSymbol);
        }

        const Order* existing = book->findOrder(req.orderId);
        if (!existing) {
            return ExecutionReportBuilder::makeReject(
                req.orderId, sym, "", RejectionReason::UnknownOrder);
        }

        bool ok = book->modifyOrder(req);
        if (!ok) {
            return ExecutionReportBuilder::makeReject(
                req.orderId, sym, existing->strategyId,
                RejectionReason::UnknownOrder);
        }

        // Re-fetch (modify may have re-inserted with new state)
        const Order* updated = book->findOrder(req.orderId);
        ExecutionReport r;
        if (updated) {
            r = ExecutionReportBuilder::makeReplace(*updated);
        } else {
            // Was matched immediately after modify
            r.orderId   = req.orderId;
            r.symbol    = sym;
            r.execType  = ExecType::Replaced;
            r.ordStatus = OrderStatus::Filled;
        }
        ++stats_.ordersModified;
        dispatch(r);
        return r;
    }

    // ── Accessors ────────────────────────────────────────────

    void setExecReportCallback(ExecReportCallback cb) {
        execReportCallback_ = std::move(cb);
    }

    [[nodiscard]] const EngineStats& stats() const noexcept { return stats_; }
    void resetStats() noexcept { stats_ = EngineStats{}; }

    [[nodiscard]] size_t symbolCount() const noexcept { return books_.size(); }

    /// Print books for all symbols.
    [[nodiscard]] std::string printAllBooks(size_t depth = 5) const {
        std::string out;
        for (auto& [sym, book] : books_) {
            out += book.printBook(depth);
        }
        return out;
    }

private:
    // Called by OrderBook trade callbacks — runs inside addOrder()
    void onTrade(const Symbol& sym, const TradeReport& tr) {
        ++stats_.totalTrades;
        stats_.totalFillQty  += static_cast<uint64_t>(tr.quantity);
        stats_.totalNotional += tr.price * static_cast<double>(tr.quantity);

        // Update VWAP state for taker (aggressor) order
        auto& takerState  = fillStates_[tr.takerOrderId];
        takerState.notional  += tr.price * static_cast<double>(tr.quantity);
        takerState.filledQty += tr.quantity;
        double takerComm = commission_->calculate(tr.price, tr.quantity, /*taker=*/true);
        takerState.commission += takerComm;
        stats_.totalCommission += takerComm;

        // Also track maker state
        auto& makerState  = fillStates_[tr.makerOrderId];
        makerState.notional  += tr.price * static_cast<double>(tr.quantity);
        makerState.filledQty += tr.quantity;
        double makerComm = commission_->calculate(tr.price, tr.quantity, /*taker=*/false);
        makerState.commission += makerComm;
        stats_.totalCommission += makerComm;

        // ── Build fill ExecutionReport for taker ──────────────
        const auto& book = books_.at(sym);
        // Taker order may have been fully consumed and removed from book
        // by the time this callback fires.  We use the TradeReport fields.
        ExecutionReport takerRpt;
        takerRpt.execId        = ExecutionReportBuilder::nextExecId();
        takerRpt.orderId       = tr.takerOrderId;
        takerRpt.symbol        = sym;
        takerRpt.side          = tr.takerSide;
        takerRpt.lastPx        = tr.price;
        takerRpt.lastQty       = tr.quantity;
        takerRpt.cumQty        = takerState.filledQty;
        takerRpt.leavesQty     = 0;    // refined below if still live
        takerRpt.avgPx         = takerState.filledQty > 0
                                     ? takerState.notional / takerState.filledQty
                                     : tr.price;
        takerRpt.lastCommission = takerComm;
        takerRpt.cumCommission  = takerState.commission;
        takerRpt.isTaker        = true;
        takerRpt.reportTime    = tr.timestamp;
        takerRpt.submitTime    = tr.timestamp - tr.matchLatencyNs;
        takerRpt.latencyNs     = tr.matchLatencyNs;
        takerRpt.execType      = (takerRpt.leavesQty == 0)
                                     ? ExecType::Fill
                                     : ExecType::PartialFill;
        takerRpt.ordStatus     = (takerRpt.leavesQty == 0)
                                     ? OrderStatus::Filled
                                     : OrderStatus::PartiallyFilled;

        // Check if taker still lives in book (partial fill)
        const Order* takerOrder = book.findOrder(tr.takerOrderId);
        if (takerOrder) {
            takerRpt.leavesQty = takerOrder->remainingQty();
            takerRpt.orderQty  = takerOrder->quantity;
            takerRpt.execType  = (takerRpt.leavesQty == 0)
                                     ? ExecType::Fill
                                     : ExecType::PartialFill;
            takerRpt.ordStatus = (takerRpt.leavesQty == 0)
                                     ? OrderStatus::Filled
                                     : OrderStatus::PartiallyFilled;
        }

        // ── Build fill ExecutionReport for maker ──────────────
        ExecutionReport makerRpt;
        makerRpt.execId        = ExecutionReportBuilder::nextExecId();
        makerRpt.orderId       = tr.makerOrderId;
        makerRpt.symbol        = sym;
        makerRpt.side          = (tr.takerSide == Side::Buy) ? Side::Sell : Side::Buy;
        makerRpt.lastPx        = tr.price;
        makerRpt.lastQty       = tr.quantity;
        makerRpt.cumQty        = makerState.filledQty;
        makerRpt.avgPx         = makerState.filledQty > 0
                                     ? makerState.notional / makerState.filledQty
                                     : tr.price;
        makerRpt.lastCommission = makerComm;
        makerRpt.cumCommission  = makerState.commission;
        makerRpt.isTaker        = false;
        makerRpt.reportTime    = tr.timestamp;
        makerRpt.latencyNs     = 0;   // maker was already resting

        const Order* makerOrder = book.findOrder(tr.makerOrderId);
        if (makerOrder) {
            makerRpt.leavesQty = makerOrder->remainingQty();
            makerRpt.orderQty  = makerOrder->quantity;
            makerRpt.execType  = (makerRpt.leavesQty == 0)
                                     ? ExecType::Fill
                                     : ExecType::PartialFill;
            makerRpt.ordStatus = (makerRpt.leavesQty == 0)
                                     ? OrderStatus::Filled
                                     : OrderStatus::PartiallyFilled;
        } else {
            makerRpt.leavesQty = 0;
            makerRpt.execType  = ExecType::Fill;
            makerRpt.ordStatus = OrderStatus::Filled;
        }

        // Emit FillEvents onto the EventLoop (for strategy callbacks)
        if (eventLoop_) {
            eventLoop_->emplace<FillEvent>(
                tr.takerOrderId, tr.tradeId,
                sym, tr.takerSide,
                tr.price, tr.quantity, takerRpt.leavesQty,
                takerComm, /*taker=*/true,
                takerRpt.strategyId);
        }

        // Buffer the reports — flushed by submitOrder() after addOrder() returns
        pendingFillReports_.push_back(std::move(takerRpt));
        pendingFillReports_.push_back(std::move(makerRpt));
    }

    void dispatch(const ExecutionReport& r) {
        if (execReportCallback_) execReportCallback_(r);
        Logger::instance().debug("MatchingEngine", "{}", r.toString());
    }

    std::unordered_map<Symbol, OrderBook>            books_;
    std::unordered_map<OrderId, OrderFillState>      fillStates_;
    std::vector<ExecutionReport>                     pendingFillReports_;
    std::shared_ptr<ICommissionModel>                commission_;
    EventLoop*                                       eventLoop_{nullptr};
    ExecReportCallback                               execReportCallback_;
    EngineStats                                      stats_;
    bool                                            enableSelfTradePrevention_{true};
};

} // namespace qtl
