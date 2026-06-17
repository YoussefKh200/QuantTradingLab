#pragma once
/**
 * @file portfolio/pnl/PnLTracker.hpp
 * @brief Real-time P&L attribution across symbols, strategies, and time.
 *
 * Tracks:
 *  - Daily / MTD / YTD / Inception-to-date realised P&L
 *  - Per-symbol and per-strategy P&L breakdown
 *  - Running equity curve (one sample per mark call)
 *  - High-water mark and current drawdown
 *  - Return on equity, Sharpe (running), win rate
 *
 * P&L components:
 *   Trading P&L  = realised P&L from fills (FIFO)
 *   MTM P&L      = unrealised P&L from mark-to-market
 *   Commission   = negative (brokerage costs)
 *   Total P&L    = Trading + MTM - Commission
 */

#include "core/Types.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <limits>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// PnLSnapshot — one point in the equity curve
// ─────────────────────────────────────────────────────────────

struct PnLSnapshot {
    Timestamp timestamp{0};
    double    equity{0.0};          ///< Cash + unrealised mark
    double    realisedPnl{0.0};
    double    unrealisedPnl{0.0};
    double    dailyPnl{0.0};
    double    commission{0.0};
    double    drawdownPct{0.0};     ///< From high-water mark
};

// ─────────────────────────────────────────────────────────────
// StrategyPnL — per-strategy attribution
// ─────────────────────────────────────────────────────────────

struct StrategyPnL {
    std::string strategyId;
    double realisedPnl{0.0};
    double commission{0.0};
    uint64_t wins{0};
    uint64_t losses{0};
    double grossProfit{0.0};
    double grossLoss{0.0};

    [[nodiscard]] double netPnl()       const noexcept { return realisedPnl - commission; }
    [[nodiscard]] double winRate()      const noexcept {
        uint64_t t = wins + losses;
        return t > 0 ? static_cast<double>(wins) / t : 0.0;
    }
    [[nodiscard]] double profitFactor() const noexcept {
        return grossLoss > 0 ? grossProfit / grossLoss : 0.0;
    }
    [[nodiscard]] double avgWin()       const noexcept {
        return wins > 0 ? grossProfit / wins : 0.0;
    }
    [[nodiscard]] double avgLoss()      const noexcept {
        return losses > 0 ? grossLoss / losses : 0.0;
    }
};

// ─────────────────────────────────────────────────────────────
// PnLTracker
// ─────────────────────────────────────────────────────────────

class PnLTracker {
public:
    explicit PnLTracker(double initialCapital = 100'000.0,
                         size_t maxHistory    = 10'000)
        : initialCapital_{initialCapital}
        , cash_{initialCapital}
        , maxHistory_{maxHistory}
    {
        highWaterMark_ = initialCapital;
    }

    // ── Event processing ──────────────────────────────────────

    /**
     * @brief Record a fill's P&L impact.
     *
     * @param symbol      Instrument
     * @param side        Buy or Sell
     * @param fillPrice   Execution price
     * @param fillQty     Quantity
     * @param commission  Cost
     * @param realisedPnl FIFO realised P&L from this fill
     * @param strategyId  Attribution tag
     */
    void onFill(const Symbol& symbol,
                Side          side,
                Price         fillPrice,
                Quantity      fillQty,
                double        commission,
                double        realisedPnl,
                const std::string& strategyId = "") {
        std::lock_guard lock{mutex_};

        // Cash changes
        double notional = fillPrice * static_cast<double>(fillQty);
        if (side == Side::Buy)  cash_ -= notional + commission;
        else                    cash_ += notional - commission;

        realisedPnl_         += realisedPnl;
        dailyRealisedPnl_    += realisedPnl;
        totalCommission_     += commission;

        // Per-symbol P&L
        symPnl_[symbol].realisedPnl  += realisedPnl;
        symPnl_[symbol].commission   += commission;

        // Per-strategy attribution
        if (!strategyId.empty()) {
            auto& sp = stratPnl_[strategyId];
            sp.strategyId = strategyId;
            sp.realisedPnl  += realisedPnl;
            sp.commission   += commission;
            if (realisedPnl > 0) { ++sp.wins;   sp.grossProfit += realisedPnl; }
            if (realisedPnl < 0) { ++sp.losses; sp.grossLoss   -= realisedPnl; }
        }

        ++fillCount_;
    }

    /**
     * @brief Mark-to-market update — updates unrealised P&L and equity curve.
     *
     * @param unrealisedPnl  Current total unrealised P&L (from PositionManager)
     */
    void onMark(double unrealisedPnl) {
        std::lock_guard lock{mutex_};
        unrealisedPnl_ = unrealisedPnl;
        double equity   = cash_ + unrealisedPnl;

        // High-water mark and drawdown
        if (equity > highWaterMark_) highWaterMark_ = equity;
        double dd = highWaterMark_ > 0
                        ? (equity - highWaterMark_) / highWaterMark_
                        : 0.0;
        currentDrawdown_ = dd;
        if (dd < maxDrawdown_) maxDrawdown_ = dd;

        // Append to equity curve
        PnLSnapshot snap;
        snap.timestamp    = nowNs();
        snap.equity       = equity;
        snap.realisedPnl  = realisedPnl_;
        snap.unrealisedPnl= unrealisedPnl;
        snap.dailyPnl     = dailyRealisedPnl_ + unrealisedPnl;
        snap.commission   = totalCommission_;
        snap.drawdownPct  = dd * 100.0;

        equityCurve_.push_back(snap);
        if (equityCurve_.size() > maxHistory_) equityCurve_.pop_front();
    }

    /**
     * @brief Reset daily P&L counter (call at session open).
     */
    void resetDailyPnl() noexcept {
        std::lock_guard lock{mutex_};
        dailyRealisedPnl_ = 0.0;
    }

    // ── Queries ───────────────────────────────────────────────

    [[nodiscard]] double cash()             const noexcept { return cash_;            }
    [[nodiscard]] double realisedPnl()      const noexcept { return realisedPnl_;     }
    [[nodiscard]] double unrealisedPnl()    const noexcept { return unrealisedPnl_;   }
    [[nodiscard]] double totalPnl()         const noexcept { return realisedPnl_ + unrealisedPnl_; }
    [[nodiscard]] double dailyPnl()         const noexcept { return dailyRealisedPnl_ + unrealisedPnl_; }
    [[nodiscard]] double totalCommission()  const noexcept { return totalCommission_;  }
    [[nodiscard]] double highWaterMark()    const noexcept { return highWaterMark_;    }
    [[nodiscard]] double currentDrawdown()  const noexcept { return currentDrawdown_;  }
    [[nodiscard]] double maxDrawdown()      const noexcept { return maxDrawdown_;      }
    [[nodiscard]] double initialCapital()   const noexcept { return initialCapital_;   }
    [[nodiscard]] double currentEquity()    const noexcept {
        return cash_ + unrealisedPnl_;
    }
    [[nodiscard]] double totalReturn()      const noexcept {
        return (initialCapital_ > 0)
                   ? (currentEquity() - initialCapital_) / initialCapital_
                   : 0.0;
    }
    [[nodiscard]] uint64_t fillCount()      const noexcept { return fillCount_;        }

    [[nodiscard]] std::vector<double> equityVector() const {
        std::lock_guard lock{mutex_};
        std::vector<double> v;
        v.reserve(equityCurve_.size());
        for (auto& s : equityCurve_) v.push_back(s.equity);
        return v;
    }

    [[nodiscard]] StrategyPnL strategyPnl(const std::string& id) const {
        std::lock_guard lock{mutex_};
        auto it = stratPnl_.find(id);
        return it == stratPnl_.end() ? StrategyPnL{} : it->second;
    }

    [[nodiscard]] std::vector<StrategyPnL> allStrategyPnl() const {
        std::lock_guard lock{mutex_};
        std::vector<StrategyPnL> out;
        out.reserve(stratPnl_.size());
        for (auto& [k, v] : stratPnl_) out.push_back(v);
        return out;
    }

    [[nodiscard]] double symbolPnl(const Symbol& sym) const {
        std::lock_guard lock{mutex_};
        auto it = symPnl_.find(sym);
        return it == symPnl_.end() ? 0.0 : it->second.realisedPnl;
    }

    /**
     * @brief Running annualised Sharpe from equity curve returns.
     * @param riskFreeRate Annual risk-free rate
     */
    [[nodiscard]] double runningSharpe(double riskFreeRate = 0.0) const {
        std::lock_guard lock{mutex_};
        if (equityCurve_.size() < 2) return 0.0;

        std::vector<double> returns;
        returns.reserve(equityCurve_.size() - 1);
        for (size_t i = 1; i < equityCurve_.size(); ++i) {
            double prev = equityCurve_[i-1].equity;
            double curr = equityCurve_[i].equity;
            if (prev > 0) returns.push_back(curr / prev - 1.0);
        }
        if (returns.empty()) return 0.0;

        double mean = std::accumulate(returns.begin(), returns.end(), 0.0) /
                      returns.size();
        double sq = 0;
        for (double r : returns) { double d = r - mean; sq += d * d; }
        double std = returns.size() > 1 ? std::sqrt(sq / (returns.size()-1)) : 0.0;
        if (std == 0) return 0.0;

        return (mean - riskFreeRate / 252.0) / std * std::sqrt(252.0);
    }

    /**
     * @brief Format a P&L summary report.
     */
    [[nodiscard]] std::string printSummary() const {
        std::lock_guard lock{mutex_};
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "╔══════════════════════════════════════════════╗\n"
            << "║              P&L SUMMARY                    ║\n"
            << "╠══════════════════════════════════════════════╣\n"
            << "║  Initial Capital : $" << std::setw(12) << initialCapital_ << "          ║\n"
            << "║  Cash            : $" << std::setw(12) << cash_           << "          ║\n"
            << "║  Current Equity  : $" << std::setw(12) << currentEquity() << "          ║\n"
            << "║  Realised P&L    : $" << std::setw(12) << realisedPnl_   << "          ║\n"
            << "║  Unrealised P&L  : $" << std::setw(12) << unrealisedPnl_ << "          ║\n"
            << "║  Total P&L       : $" << std::setw(12) << totalPnl()     << "          ║\n"
            << "║  Commission      : $" << std::setw(12) << totalCommission_<< "          ║\n"
            << "║  Total Return    : "  << std::setw(12) << (totalReturn()*100) << "%         ║\n"
            << "║  Max Drawdown    : "  << std::setw(12) << (maxDrawdown_*100)  << "%         ║\n"
            << "║  High-Water Mark : $" << std::setw(12) << highWaterMark_ << "          ║\n"
            << "║  Fill Count      : "  << std::setw(13) << fillCount_     << "          ║\n"
            << "╠══════════════════════════════════════════════╣\n";

        if (!stratPnl_.empty()) {
            oss << "║  STRATEGY ATTRIBUTION                        ║\n";
            for (auto& [id, sp] : stratPnl_) {
                oss << "║  " << std::setw(16) << std::left << id
                    << " P&L=$" << std::setw(10) << std::right << sp.netPnl()
                    << " WR=" << std::setprecision(0) << (sp.winRate()*100) << "%  ║\n";
                oss << std::setprecision(2);
            }
        }
        oss << "╚══════════════════════════════════════════════╝\n";
        return oss.str();
    }

private:
    double   initialCapital_;
    double   cash_;
    double   realisedPnl_{0.0};
    double   unrealisedPnl_{0.0};
    double   dailyRealisedPnl_{0.0};
    double   totalCommission_{0.0};
    double   highWaterMark_;
    double   currentDrawdown_{0.0};
    double   maxDrawdown_{0.0};
    uint64_t fillCount_{0};
    size_t   maxHistory_;

    std::unordered_map<Symbol,      StrategyPnL> symPnl_;
    std::unordered_map<std::string, StrategyPnL> stratPnl_;
    std::deque<PnLSnapshot>                      equityCurve_;
    mutable std::mutex                           mutex_;
};

} // namespace qtl
