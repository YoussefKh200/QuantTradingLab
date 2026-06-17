#pragma once
/**
 * @file portfolio/accounting/PortfolioAccountant.hpp
 * @brief Portfolio-level accounting — the facade unifying positions and P&L.
 *
 * PortfolioAccountant is the single entry point for all portfolio operations.
 * It owns a PositionManager and a PnLTracker, and coordinates them:
 *
 *   onFill()  → PositionManager::onFill()  (update position)
 *              → PnLTracker::onFill()       (update P&L)
 *
 *   onMark()  → PositionManager::mark()     (update MTM price)
 *              → PnLTracker::onMark()        (update equity curve)
 *
 *   onFillEvent()  — convenience wrapper for FillEvent from EventLoop
 *   onMarketEvent()— convenience wrapper for MarketEvent from EventLoop
 *
 * Portfolio exposure
 * ──────────────────
 * Provides real-time gross/net exposure:
 *   grossExposure = Σ |position_i × price_i|
 *   netExposure   = Σ  position_i × price_i
 *   leverage      = grossExposure / nav
 *
 * Multi-strategy support
 * ──────────────────────
 * Every fill is attributed to a strategyId. Queries can be filtered
 * by strategy for P&L attribution and position-level reporting.
 */

#include "portfolio/positions/PositionManager.hpp"
#include "portfolio/pnl/PnLTracker.hpp"
#include "core/events/Event.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <sstream>
#include <iomanip>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// PortfolioExposureSnapshot — real-time portfolio exposures
// ─────────────────────────────────────────────────────────────

struct PortfolioExposureSnapshot {
    double grossExposure{0.0};   ///< Sum |qty × price| across all positions
    double netExposure{0.0};     ///< Sum (qty × price) with sign
    double longExposure{0.0};    ///< Total long notional
    double shortExposure{0.0};   ///< Total short notional (positive)
    double nav{0.0};             ///< Net asset value (equity)
    double grossLeverage{0.0};   ///< grossExposure / nav
    double netLeverage{0.0};     ///< |netExposure| / nav
    size_t symbolCount{0};       ///< Distinct symbols with open positions
    size_t positionCount{0};     ///< Total open positions (symbol × strategy)

    [[nodiscard]] std::string toString() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "Portfolio Exposure:"
            << " gross=$" << grossExposure
            << " net=$"   << netExposure
            << " long=$"  << longExposure
            << " short=$" << shortExposure
            << " lev="    << grossLeverage << "x"
            << " syms="   << symbolCount;
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────
// PortfolioAccountant
// ─────────────────────────────────────────────────────────────

class PortfolioAccountant {
public:
    explicit PortfolioAccountant(double initialCapital    = 100'000.0,
                                  bool   allowShortSelling = true)
        : positions_{allowShortSelling}
        , pnl_{initialCapital}
        , nav_{initialCapital}
    {}

    // ── Core event processing ─────────────────────────────────

    /**
     * @brief Process a fill — updates positions and P&L atomically.
     *
     * @param symbol      Instrument
     * @param side        Buy or Sell
     * @param fillPrice   Execution price
     * @param fillQty     Quantity filled (positive)
     * @param commission  Brokerage cost
     * @param strategyId  Attribution tag
     */
    void onFill(const Symbol& symbol,
                Side          side,
                Price         fillPrice,
                Quantity      fillQty,
                double        commission  = 0.0,
                const std::string& strategyId = "") {
        // 1. Update positions (returns FIFO realised P&L)
        double realisedPnl = positions_.onFill(
            symbol, side, fillPrice, fillQty, commission, strategyId);

        // 2. Update P&L tracker
        pnl_.onFill(symbol, side, fillPrice, fillQty,
                    commission, realisedPnl, strategyId);

        // 3. Update last price cache
        lastPrices_[symbol] = fillPrice;
    }

    /**
     * @brief Convenience wrapper for FillEvent from EventLoop.
     */
    void onFill(const FillEvent& e) {
        onFill(e.symbol, e.side, e.fillPrice, e.fillQuantity,
               e.commission, e.strategyId);
    }

    /**
     * @brief Mark-to-market update from a market event.
     *
     * @param symbol     Instrument
     * @param markPrice  Current price (bid/ask mid or last trade)
     */
    void onMark(const Symbol& symbol, Price markPrice) {
        if (markPrice <= 0) return;
        lastPrices_[symbol] = markPrice;
        positions_.mark(symbol, markPrice);

        // Recompute total unrealised P&L and update equity curve
        double unrealisedTotal = positions_.totalUnrealisedPnl();
        nav_ = pnl_.cash() + unrealisedTotal;
        pnl_.onMark(unrealisedTotal);
    }

    /**
     * @brief Convenience wrapper for MarketEvent.
     */
    void onMark(const MarketEvent& e) {
        Price markPrice = e.lastPrice > 0 ? e.lastPrice
                          : (e.bidPrice + e.askPrice) / 2.0;
        if (markPrice > 0) onMark(e.symbol, markPrice);
    }

    /**
     * @brief Reset daily P&L counter.
     */
    void resetDailyPnl() { pnl_.resetDailyPnl(); }

    // ── Position queries ──────────────────────────────────────

    [[nodiscard]] Quantity netQty(const Symbol& sym) const {
        return positions_.netQty(sym);
    }
    [[nodiscard]] Position getPosition(const Symbol& sym,
                                        const std::string& strat = "") const {
        return positions_.getPosition(sym, strat);
    }
    [[nodiscard]] std::vector<Position> openPositions() const {
        return positions_.openPositions();
    }
    [[nodiscard]] size_t openPositionCount() const {
        return positions_.openPositionCount();
    }

    // ── P&L queries ───────────────────────────────────────────

    [[nodiscard]] double cash()             const noexcept { return pnl_.cash();           }
    [[nodiscard]] double nav()              const noexcept { return nav_;                   }
    [[nodiscard]] double realisedPnl()      const noexcept { return pnl_.realisedPnl();    }
    [[nodiscard]] double unrealisedPnl()    const noexcept { return pnl_.unrealisedPnl();  }
    [[nodiscard]] double totalPnl()         const noexcept { return pnl_.totalPnl();       }
    [[nodiscard]] double dailyPnl()         const noexcept { return pnl_.dailyPnl();       }
    [[nodiscard]] double totalCommission()  const noexcept { return pnl_.totalCommission(); }
    [[nodiscard]] double totalReturn()      const noexcept { return pnl_.totalReturn();    }
    [[nodiscard]] double maxDrawdown()      const noexcept { return pnl_.maxDrawdown();    }
    [[nodiscard]] double currentDrawdown()  const noexcept { return pnl_.currentDrawdown();}
    [[nodiscard]] double highWaterMark()    const noexcept { return pnl_.highWaterMark();  }
    [[nodiscard]] double runningSharpe()    const noexcept { return pnl_.runningSharpe();  }
    [[nodiscard]] std::vector<double> equityVector() const {
        return pnl_.equityVector();
    }
    [[nodiscard]] StrategyPnL strategyPnl(const std::string& id) const {
        return pnl_.strategyPnl(id);
    }
    [[nodiscard]] std::vector<StrategyPnL> allStrategyPnl() const {
        return pnl_.allStrategyPnl();
    }

    // ── Exposure ──────────────────────────────────────────────

    /**
     * @brief Compute current portfolio exposure snapshot.
     */
    [[nodiscard]] PortfolioExposureSnapshot exposure() const {
        PortfolioExposureSnapshot snap;
        snap.nav          = nav_;
        snap.positionCount= positions_.openPositionCount();

        auto positions = positions_.openPositions();
        for (auto& pos : positions) {
            Price price = pos.lastPrice > 0 ? pos.lastPrice : pos.avgEntryPrice;
            double notional = price * static_cast<double>(std::abs(pos.netQty));
            double signedNotional = price * static_cast<double>(pos.netQty);

            snap.grossExposure += notional;
            snap.netExposure   += signedNotional;

            if (pos.isLong())  snap.longExposure  += notional;
            else               snap.shortExposure += notional;
        }

        // Count distinct symbols
        auto syms = positions_.allSymbols();
        snap.symbolCount = syms.size();

        if (snap.nav > 0) {
            snap.grossLeverage = snap.grossExposure / snap.nav;
            snap.netLeverage   = std::abs(snap.netExposure) / snap.nav;
        }
        return snap;
    }

    // ── Reporting ─────────────────────────────────────────────

    [[nodiscard]] std::string printPositions() const {
        return positions_.printPositions();
    }

    [[nodiscard]] std::string printPnL() const {
        return pnl_.printSummary();
    }

    [[nodiscard]] std::string printFull() const {
        auto exp = exposure();
        std::ostringstream oss;
        oss << printPnL() << "\n"
            << printPositions() << "\n"
            << "Exposure: " << exp.toString() << "\n";
        return oss.str();
    }

    void reset() {
        positions_.reset();
    }

private:
    PositionManager                      positions_;
    PnLTracker                           pnl_;
    double                               nav_;
    std::unordered_map<Symbol, Price>    lastPrices_;
};

} // namespace qtl
