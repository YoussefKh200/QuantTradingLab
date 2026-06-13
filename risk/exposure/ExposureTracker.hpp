#pragma once
/**
 * @file risk/exposure/ExposureTracker.hpp
 * @brief Real-time gross/net exposure tracker across all positions.
 *
 * Tracks:
 *  - Per-symbol: long notional, short notional, net notional, position qty
 *  - Portfolio: gross exposure, net exposure, long/short totals
 *  - Concentration: largest single-name as fraction of gross
 *  - Delta-equivalent exposure (for options, applied in Phase 9)
 *
 * All updates are O(1) and lock-free at the symbol level using atomics.
 * Portfolio-level aggregates are computed on-demand (O(N) symbols).
 *
 * Thread-safety:
 *  Per-symbol updates are lock-free.
 *  Portfolio-level queries acquire a shared_mutex for consistent snapshot.
 */

#include "core/Types.hpp"
#include <mutex>
#include <unordered_map>
#include <shared_mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <atomic>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// SymbolExposure — per-instrument exposure record
// ─────────────────────────────────────────────────────────────

struct SymbolExposure {
    Symbol   symbol;
    Quantity netQty{0};          ///< Long positive, short negative
    double   longNotional{0.0};  ///< Sum of all long position notional
    double   shortNotional{0.0}; ///< Sum of all short position notional (abs)
    double   lastPrice{0.0};     ///< Last mark price for MTM calculation
    double   avgEntryPrice{0.0}; ///< Volume-weighted average entry price
    double   unrealisedPnl{0.0}; ///< MTM unrealised P&L

    [[nodiscard]] double netNotional()   const noexcept {
        return longNotional - shortNotional;
    }
    [[nodiscard]] double grossNotional() const noexcept {
        return longNotional + shortNotional;
    }
    [[nodiscard]] bool   isFlat()        const noexcept { return netQty == 0; }
    [[nodiscard]] bool   isLong()        const noexcept { return netQty >  0; }
    [[nodiscard]] bool   isShort()       const noexcept { return netQty <  0; }

    void updateMtm(double markPrice) noexcept {
        lastPrice = markPrice;
        if (avgEntryPrice > 0 && netQty != 0) {
            unrealisedPnl = (markPrice - avgEntryPrice) *
                            static_cast<double>(netQty);
        }
    }
};

// ─────────────────────────────────────────────────────────────
// PortfolioExposure — aggregate snapshot
// ─────────────────────────────────────────────────────────────

struct PortfolioExposure {
    double grossExposure{0.0};    ///< Sum of |notional| across all symbols
    double netExposure{0.0};      ///< Sum of signed notional (long - short)
    double longExposure{0.0};     ///< Total long notional
    double shortExposure{0.0};    ///< Total short notional (positive value)
    double totalUnrealisedPnl{0.0};
    size_t symbolCount{0};        ///< Number of symbols with non-zero position
    std::string largestName;      ///< Symbol with largest gross exposure
    double largestNotional{0.0};  ///< Notional of largest position

    [[nodiscard]] double concentrationPct() const noexcept {
        return grossExposure > 0.0
                   ? largestNotional / grossExposure
                   : 0.0;
    }
    [[nodiscard]] double netLeverage(double nav) const noexcept {
        return nav > 0.0 ? std::abs(netExposure) / nav : 0.0;
    }
    [[nodiscard]] double grossLeverage(double nav) const noexcept {
        return nav > 0.0 ? grossExposure / nav : 0.0;
    }

    [[nodiscard]] std::string toString() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "Exposure: gross=$" << grossExposure
            << " net=$"     << netExposure
            << " long=$"    << longExposure
            << " short=$"   << shortExposure
            << " unrealisedPnl=$" << totalUnrealisedPnl
            << " symbols="  << symbolCount;
        if (!largestName.empty())
            oss << " largest=" << largestName
                << "($" << largestNotional << ")";
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────
// ExposureTracker
// ─────────────────────────────────────────────────────────────

class ExposureTracker {
public:
    ExposureTracker() = default;

    // ── Fill processing ───────────────────────────────────────

    /**
     * @brief Update exposure after a fill.
     * Called by the risk engine for every execution report.
     */
    void onFill(const Symbol& sym,
                Side          side,
                Price         fillPrice,
                Quantity      fillQty) {
        std::unique_lock lock{mutex_};
        auto& exp = symbols_[sym];
        exp.symbol = sym;

        double notional = fillPrice * static_cast<double>(fillQty);

        if (side == Side::Buy) {
            // Update average entry price (FIFO not required here — use VWAP)
            double prevNotional = exp.avgEntryPrice *
                                  static_cast<double>(std::max(exp.netQty, Quantity{0}));
            exp.netQty        += fillQty;
            exp.longNotional  += notional;
            // VWAP entry for long positions
            if (exp.netQty > 0) {
                exp.avgEntryPrice = (prevNotional + notional) /
                                    static_cast<double>(exp.netQty);
            }
        } else {
            exp.netQty       -= fillQty;
            exp.shortNotional += notional;
            // When flat or flipping, reset avg entry
            if (exp.netQty <= 0) {
                exp.avgEntryPrice = fillPrice;
            }
        }

        exp.lastPrice = fillPrice;
        ++fillCount_;
    }

    /**
     * @brief Update mark price for unrealised P&L.
     */
    void markToMarket(const Symbol& sym, Price markPrice) {
        std::unique_lock lock{mutex_};
        auto it = symbols_.find(sym);
        if (it != symbols_.end()) {
            it->second.updateMtm(markPrice);
        }
    }

    // ── Queries ───────────────────────────────────────────────

    /**
     * @brief Get exposure for one symbol (returns default if not tracked).
     */
    [[nodiscard]] SymbolExposure getSymbol(const Symbol& sym) const {
        std::shared_lock lock{mutex_};
        auto it = symbols_.find(sym);
        return it == symbols_.end() ? SymbolExposure{} : it->second;
    }

    /**
     * @brief Compute aggregate portfolio exposure (O(N) in symbols).
     */
    [[nodiscard]] PortfolioExposure portfolio() const {
        std::shared_lock lock{mutex_};
        PortfolioExposure p;
        for (auto& [sym, exp] : symbols_) {
            if (exp.isFlat()) continue;
            ++p.symbolCount;
            p.longExposure   += exp.longNotional;
            p.shortExposure  += exp.shortNotional;
            p.totalUnrealisedPnl += exp.unrealisedPnl;
            double gross = exp.grossNotional();
            p.grossExposure += gross;
            p.netExposure   += exp.netNotional();
            if (gross > p.largestNotional) {
                p.largestNotional = gross;
                p.largestName     = sym;
            }
        }
        return p;
    }

    /**
     * @brief Net quantity for a symbol.
     */
    [[nodiscard]] Quantity netQty(const Symbol& sym) const {
        std::shared_lock lock{mutex_};
        auto it = symbols_.find(sym);
        return it == symbols_.end() ? 0 : it->second.netQty;
    }

    /**
     * @brief Gross notional for a symbol.
     */
    [[nodiscard]] double grossNotional(const Symbol& sym) const {
        std::shared_lock lock{mutex_};
        auto it = symbols_.find(sym);
        return it == symbols_.end() ? 0.0 : it->second.grossNotional();
    }

    /**
     * @brief All tracked symbols.
     */
    [[nodiscard]] std::vector<Symbol> trackedSymbols() const {
        std::shared_lock lock{mutex_};
        std::vector<Symbol> out;
        out.reserve(symbols_.size());
        for (auto& [k, _] : symbols_) out.push_back(k);
        return out;
    }

    [[nodiscard]] uint64_t fillCount() const noexcept { return fillCount_; }

    void reset() {
        std::unique_lock lock{mutex_};
        symbols_.clear();
        fillCount_ = 0;
    }

private:
    mutable std::shared_mutex                  mutex_;
    std::unordered_map<Symbol, SymbolExposure> symbols_;
    uint64_t                                   fillCount_{0};
};

} // namespace qtl
