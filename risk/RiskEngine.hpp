#pragma once
/**
 * @file risk/RiskEngine.hpp
 * @brief Real-time risk engine — monitors, checks, and halts.
 *
 * Architecture
 * ────────────
 *
 *   FillEvent ──► RiskEngine::onFill()
 *                   ├── ExposureTracker::onFill()
 *                   ├── update dailyPnl / drawdown
 *                   └── checkAllLimits() ──► KillSwitch::trigger() if breached
 *
 *   Order ──────► RiskEngine::checkOrder()  (pre-trade)
 *                   ├── order size / notional / price deviation
 *                   ├── symbol position limits
 *                   ├── portfolio exposure limits
 *                   └── session loss limits
 *                   Returns PreTradeResult{approved/rejected}
 *
 *   MarketEvent ► RiskEngine::onMark()
 *                   ├── ExposureTracker::markToMarket()
 *                   └── recompute unrealised P&L / drawdown
 *
 * Severity levels
 * ───────────────
 *   INFO    — metric updated, no action
 *   WARNING — soft threshold breached, alert fired, trading continues
 *   BREACH  — hard limit breached, KillSwitch triggered
 *
 * Thread safety
 * ─────────────
 * All public methods are guarded.  The KillSwitch check (isTradingHalted)
 * is a single atomic load — zero overhead on the hot path.
 */

#include "risk/limits/RiskLimits.hpp"
#include "exchange/orderbook/Order.hpp"
#include "risk/exposure/ExposureTracker.hpp"
#include "risk/kill_switch/KillSwitch.hpp"
#include "core/events/Event.hpp"
#include "core/logger/Logger.hpp"

#include <memory>
#include <functional>
#include <atomic>
#include <mutex>
#include <chrono>
#include <vector>
#include <deque>
#include <string>
#include <sstream>
#include <algorithm>
#include <cmath>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// RiskAlert — warning notification (below kill threshold)
// ─────────────────────────────────────────────────────────────

struct RiskAlert {
    enum class Severity : uint8_t { Info, Warning, Breach };

    Severity    severity{Severity::Info};
    std::string component;
    std::string message;
    double      currentValue{0.0};
    double      limitValue{0.0};
    Timestamp   timestamp{0};

    [[nodiscard]] std::string toString() const {
        static constexpr const char* kSev[] = {"INFO","WARNING","BREACH"};
        std::ostringstream oss;
        oss << "[" << kSev[static_cast<int>(severity)] << "] "
            << component << ": " << message
            << " (value=" << currentValue
            << " limit=" << limitValue << ")";
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────
// RiskEngineStats
// ─────────────────────────────────────────────────────────────

struct RiskEngineStats {
    uint64_t fillsProcessed{0};
    uint64_t ordersApproved{0};
    uint64_t ordersRejected{0};
    uint64_t warningsFired{0};
    uint64_t breachesFired{0};
    double   peakEquity{0.0};
    double   dailyPnl{0.0};
    double   realisedPnl{0.0};
    double   unrealisedPnl{0.0};
    double   currentDrawdownPct{0.0};
    double   maxDrawdownSeen{0.0};
    int      orderRateCurrent{0};   ///< Orders in the last second
};

// ─────────────────────────────────────────────────────────────
// RiskEngine
// ─────────────────────────────────────────────────────────────

class RiskEngine {
public:
    using AlertCallback = std::function<void(const RiskAlert&)>;

    explicit RiskEngine(RiskLimits                 limits      = {},
                         std::shared_ptr<KillSwitch> killSwitch  = nullptr,
                         double                      initialNav  = 100'000.0)
        : limits_{std::move(limits)}
        , ks_{killSwitch ? std::move(killSwitch)
                         : std::make_shared<KillSwitch>()}
        , nav_{initialNav}
        , peakNav_{initialNav}
    {}

    // Non-copyable
    RiskEngine(const RiskEngine&)            = delete;
    RiskEngine& operator=(const RiskEngine&) = delete;

    // ── Configuration ─────────────────────────────────────────

    void setLimits(RiskLimits limits)   { limits_ = std::move(limits); }
    void setAlertCallback(AlertCallback cb) { alertCb_ = std::move(cb); }
    void setNav(double nav) noexcept    { nav_ = nav; if (nav > peakNav_) peakNav_ = nav; }

    [[nodiscard]] KillSwitch& killSwitch() noexcept { return *ks_; }
    [[nodiscard]] const KillSwitch& killSwitch() const noexcept { return *ks_; }
    [[nodiscard]] const ExposureTracker& exposure() const noexcept { return tracker_; }
    [[nodiscard]] const RiskLimits&      limits()   const noexcept { return limits_; }
    [[nodiscard]] const RiskEngineStats& stats()    const noexcept { return stats_; }

    // ── Pre-trade check ───────────────────────────────────────

    /**
     * @brief Check an order before submission.
     *
     * Returns PreTradeResult::ok() if all checks pass.
     * Returns PreTradeResult::reject(reason) if any limit is breached.
     * Does NOT modify state — pure read.
     */
    [[nodiscard]] PreTradeResult checkOrder(const Order& order) {
        std::shared_lock lock{mutex_};

        // 0. Kill switch — immediate reject if halted
        if (ks_->isTradingHalted()) {
            ++stats_.ordersRejected;
            return PreTradeResult::reject("Trading halted: " +
                killReasonName(ks_->reason()));
        }

        // 1. Market orders allowed?
        if (order.isMarket() && !limits_.order.allowMarketOrders) {
            ++stats_.ordersRejected;
            return PreTradeResult::reject("Market orders disabled");
        }

        // 2. Short selling allowed?
        if (!limits_.order.allowShortSelling && order.isSell()) {
            Quantity pos = tracker_.netQty(order.symbol);
            if (pos - order.quantity < 0) {
                ++stats_.ordersRejected;
                return PreTradeResult::reject("Short selling disabled");
            }
        }

        // 3. Order size limit
        if (order.quantity > limits_.order.maxOrderQty) {
            ++stats_.ordersRejected;
            return PreTradeResult::reject(
                "Order qty " + std::to_string(order.quantity) +
                " > max " + std::to_string(limits_.order.maxOrderQty));
        }

        // 4. Order notional limit
        double notional = order.price * static_cast<double>(order.quantity);
        if (order.isLimit() && notional > limits_.order.maxOrderNotional) {
            ++stats_.ordersRejected;
            return PreTradeResult::reject(
                "Order notional $" + std::to_string(notional) +
                " > max $" + std::to_string(limits_.order.maxOrderNotional));
        }

        // 5. Symbol-level position limit
        auto symLimits  = limits_.getSymbolLimits(order.symbol);
        if (!symLimits.tradingEnabled) {
            ++stats_.ordersRejected;
            return PreTradeResult::reject("Trading disabled for " + order.symbol);
        }

        Quantity curQty = tracker_.netQty(order.symbol);
        if (order.isBuy()) {
            if (curQty + order.quantity > symLimits.maxLongQty) {
                ++stats_.ordersRejected;
                return PreTradeResult::reject(
                    order.symbol + " long limit: " +
                    std::to_string(curQty + order.quantity) +
                    " > " + std::to_string(symLimits.maxLongQty));
            }
        } else {
            if (std::abs(curQty - order.quantity) > symLimits.maxShortQty) {
                ++stats_.ordersRejected;
                return PreTradeResult::reject(
                    order.symbol + " short limit: " +
                    std::to_string(std::abs(curQty - order.quantity)) +
                    " > " + std::to_string(symLimits.maxShortQty));
            }
        }

        // 6. Portfolio gross exposure
        auto port = tracker_.portfolio();
        double addedNotional = std::abs(notional);
        if (port.grossExposure + addedNotional > limits_.portfolio.maxGrossExposure) {
            ++stats_.ordersRejected;
            return PreTradeResult::reject(
                "Gross exposure $" +
                std::to_string(port.grossExposure + addedNotional) +
                " > limit $" + std::to_string(limits_.portfolio.maxGrossExposure));
        }

        // 7. Daily loss — block new orders if near limit
        if (stats_.dailyPnl < limits_.session.softDailyLossWarning) {
            ++stats_.ordersRejected;
            return PreTradeResult::reject(
                "Daily loss $" + std::to_string(stats_.dailyPnl) +
                " at warning threshold");
        }

        // 8. Order rate limit
        if (stats_.orderRateCurrent >= limits_.order.maxOrdersPerSecond) {
            ++stats_.ordersRejected;
            return PreTradeResult::reject("Order rate limit exceeded");
        }

        ++stats_.ordersApproved;
        return PreTradeResult::ok();
    }

    // ── Post-trade processing ─────────────────────────────────

    /**
     * @brief Process a fill — update exposure and P&L, then check limits.
     */
    void onFill(const Symbol& sym,
                Side          side,
                Price         fillPrice,
                Quantity      fillQty,
                double        commission) {
        {
            std::unique_lock lock{mutex_};
            tracker_.onFill(sym, side, fillPrice, fillQty);
            ++stats_.fillsProcessed;

            // Approximate realised P&L contribution
            double notional = fillPrice * static_cast<double>(fillQty);
            double pnlDelta = (side == Side::Sell ? +1.0 : -1.0) *
                              notional - commission;
            stats_.realisedPnl += pnlDelta;
            stats_.dailyPnl    += pnlDelta;

            recordOrderTimestamp();
        }
        checkAllLimits("onFill");
    }

    /**
     * @brief Process a FillEvent from the EventLoop.
     */
    void onFill(const FillEvent& e) {
        onFill(e.symbol, e.side, e.fillPrice, e.fillQuantity, e.commission);
    }

    /**
     * @brief Mark-to-market update — recalculate unrealised P&L and drawdown.
     */
    void onMark(const Symbol& sym, Price markPrice, double portfolioEquity) {
        {
            std::unique_lock lock{mutex_};
            tracker_.markToMarket(sym, markPrice);
            setNav(portfolioEquity);

            // Update unrealised P&L from tracker
            auto port = tracker_.portfolio();
            stats_.unrealisedPnl = port.totalUnrealisedPnl;

            // Drawdown
            if (portfolioEquity > stats_.peakEquity)
                stats_.peakEquity = portfolioEquity;

            if (stats_.peakEquity > 0) {
                stats_.currentDrawdownPct =
                    (portfolioEquity - stats_.peakEquity) / stats_.peakEquity;
                if (stats_.currentDrawdownPct < stats_.maxDrawdownSeen)
                    stats_.maxDrawdownSeen = stats_.currentDrawdownPct;
            }
        }
        checkAllLimits("onMark");
    }

    /**
     * @brief Reset daily P&L at session start.
     */
    void resetDailyPnl() noexcept {
        std::unique_lock lock{mutex_};
        stats_.dailyPnl = 0.0;
        orderTimestamps_.clear();
    }

    /**
     * @brief Manually trigger the kill switch.
     */
    void manualHalt(const std::string& reason = "Manual operator halt") {
        ks_->trigger(KillReason::ManualHalt, reason, "Operator");
    }

    // ── Alert history ─────────────────────────────────────────

    [[nodiscard]] std::vector<RiskAlert> recentAlerts(size_t n = 20) const {
        std::shared_lock lock{mutex_};
        size_t start = alertHistory_.size() > n
                           ? alertHistory_.size() - n : 0;
        return {alertHistory_.begin() + start, alertHistory_.end()};
    }

private:
    // ── Limit checking ────────────────────────────────────────

    void checkAllLimits(const char* context) {
        if (ks_->isTradingHalted()) return;  // already halted

        std::shared_lock lock{mutex_};
        auto port = tracker_.portfolio();

        // Daily loss hard breach
        if (stats_.dailyPnl <= limits_.session.maxDailyLoss) {
            lock.unlock();
            ks_->trigger(KillReason::DailyLossLimit,
                "Daily P&L $" + std::to_string(stats_.dailyPnl) +
                " <= limit $" + std::to_string(limits_.session.maxDailyLoss),
                context,
                stats_.dailyPnl,
                limits_.session.maxDailyLoss);
            fireAlert(RiskAlert::Severity::Breach, context,
                "Daily loss limit breached",
                stats_.dailyPnl, limits_.session.maxDailyLoss);
            ++stats_.breachesFired;
            return;
        }

        // Max drawdown breach
        if (stats_.currentDrawdownPct <= limits_.session.maxDrawdownPct) {
            lock.unlock();
            ks_->trigger(KillReason::MaxDrawdown,
                "Drawdown " +
                std::to_string(stats_.currentDrawdownPct * 100.0) + "%" +
                " <= limit " +
                std::to_string(limits_.session.maxDrawdownPct * 100.0) + "%",
                context,
                stats_.currentDrawdownPct,
                limits_.session.maxDrawdownPct);
            fireAlert(RiskAlert::Severity::Breach, context,
                "Max drawdown breached",
                stats_.currentDrawdownPct, limits_.session.maxDrawdownPct);
            ++stats_.breachesFired;
            return;
        }

        // Gross exposure breach
        if (port.grossExposure > limits_.portfolio.maxGrossExposure) {
            lock.unlock();
            ks_->trigger(KillReason::GrossExposure,
                "Gross exposure $" + std::to_string(port.grossExposure) +
                " > limit $" + std::to_string(limits_.portfolio.maxGrossExposure),
                context,
                port.grossExposure, limits_.portfolio.maxGrossExposure);
            ++stats_.breachesFired;
            return;
        }

        // Concentration breach
        if (port.concentrationPct() > limits_.portfolio.maxConcentrationPct) {
            fireAlert(RiskAlert::Severity::Warning, context,
                "Concentration " +
                std::to_string(port.concentrationPct() * 100.0) + "%" +
                " in " + port.largestName,
                port.concentrationPct(), limits_.portfolio.maxConcentrationPct);
            ++stats_.warningsFired;
        }

        // Soft daily loss warning
        if (stats_.dailyPnl <= limits_.session.softDailyLossWarning &&
            stats_.dailyPnl > limits_.session.maxDailyLoss) {
            fireAlert(RiskAlert::Severity::Warning, context,
                "Daily loss warning: $" + std::to_string(stats_.dailyPnl),
                stats_.dailyPnl, limits_.session.softDailyLossWarning);
            ++stats_.warningsFired;
        }

        // Soft drawdown warning
        if (stats_.currentDrawdownPct <= limits_.session.softDrawdownWarning &&
            stats_.currentDrawdownPct > limits_.session.maxDrawdownPct) {
            fireAlert(RiskAlert::Severity::Warning, context,
                "Drawdown warning: " +
                std::to_string(stats_.currentDrawdownPct * 100.0) + "%",
                stats_.currentDrawdownPct, limits_.session.softDrawdownWarning);
            ++stats_.warningsFired;
        }
    }

    void fireAlert(RiskAlert::Severity sev,
                    const std::string& component,
                    const std::string& message,
                    double current, double limit) {
        RiskAlert alert;
        alert.severity     = sev;
        alert.component    = component;
        alert.message      = message;
        alert.currentValue = current;
        alert.limitValue   = limit;
        alert.timestamp    = nowNs();

        alertHistory_.push_back(alert);
        if (alertHistory_.size() > 500) alertHistory_.erase(alertHistory_.begin());

        Logger::instance().warn("RiskEngine", "{}", alert.toString());
        if (alertCb_) alertCb_(alert);
    }

    void recordOrderTimestamp() {
        Timestamp now = nowNs();
        // Remove timestamps older than 1 second
        while (!orderTimestamps_.empty() &&
               now - orderTimestamps_.front() > 1'000'000'000LL) {
            orderTimestamps_.pop_front();
        }
        orderTimestamps_.push_back(now);
        stats_.orderRateCurrent = static_cast<int>(orderTimestamps_.size());
    }

    RiskLimits                   limits_;
    std::shared_ptr<KillSwitch>  ks_;
    ExposureTracker              tracker_;
    RiskEngineStats              stats_;
    double                       nav_{100'000.0};
    double                       peakNav_{100'000.0};
    AlertCallback                alertCb_;
    std::vector<RiskAlert>       alertHistory_;
    std::deque<Timestamp>        orderTimestamps_;  ///< For rate limiting
    mutable std::shared_mutex    mutex_;
};

} // namespace qtl
