#pragma once
/**
 * @file risk/limits/RiskLimits.hpp
 * @brief Configurable risk limit definitions.
 *
 * Limits are plain-data structs configured at startup.
 * The RiskEngine reads them to decide whether to fire the KillSwitch.
 *
 * Hierarchy of limits (checked in order of severity):
 *   1. Per-order pre-trade checks  (OrderLimits)
 *   2. Per-symbol position limits  (SymbolLimits)
 *   3. Portfolio-level limits      (PortfolioLimits)
 *   4. Daily / session limits      (SessionLimits)
 */

#include "core/Types.hpp"
#include <string>
#include <unordered_map>
#include <limits>
#include <sstream>
#include <iomanip>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// OrderLimits — per-order pre-trade validation
// ─────────────────────────────────────────────────────────────

struct OrderLimits {
    Quantity maxOrderQty{10'000};         ///< Max qty on any single order
    double   maxOrderNotional{1'000'000}; ///< Max $ value of any single order
    double   maxPriceDeviationPct{0.05};  ///< Max % deviation from last price
    int      maxOrdersPerSecond{100};     ///< Rate limit for order submission
    bool     allowMarketOrders{true};     ///< Whether market orders are permitted
    bool     allowShortSelling{true};     ///< Whether short positions are permitted
};

// ─────────────────────────────────────────────────────────────
// SymbolLimits — per-instrument position limits
// ─────────────────────────────────────────────────────────────

struct SymbolLimits {
    Symbol   symbol;
    Quantity maxLongQty{100'000};          ///< Maximum long position size
    Quantity maxShortQty{100'000};         ///< Maximum short position size (abs)
    double   maxGrossNotional{10'000'000}; ///< Max gross $ exposure per symbol
    double   maxConcentrationPct{0.20};    ///< Max % of portfolio in this name
    bool     tradingEnabled{true};         ///< Can be disabled per-symbol
};

// ─────────────────────────────────────────────────────────────
// PortfolioLimits — aggregate portfolio constraints
// ─────────────────────────────────────────────────────────────

struct PortfolioLimits {
    double maxGrossExposure{50'000'000};  ///< Total gross $ across all positions
    double maxNetExposure{20'000'000};    ///< Max abs net long/short $
    double maxGrossLeverage{4.0};         ///< Gross exposure / NAV
    double maxNetLeverage{2.0};           ///< Net exposure / NAV
    double maxConcentrationPct{0.30};     ///< Largest single name as % of gross
    int    maxSymbolCount{50};            ///< Maximum number of open positions
};

// ─────────────────────────────────────────────────────────────
// SessionLimits — daily / session-level loss and drawdown
// ─────────────────────────────────────────────────────────────

struct SessionLimits {
    double maxDailyLoss{-50'000.0};          ///< Max realised + unrealised loss/day ($)
    double maxDailyLossPct{-0.05};           ///< Max daily loss as fraction of NAV
    double maxDrawdownPct{-0.10};            ///< Max peak-to-trough drawdown on NAV
    double maxDrawdownAbs{-100'000.0};       ///< Max drawdown in $ terms
    double softDailyLossWarning{-30'000.0};  ///< Warning threshold (before hard limit)
    double softDrawdownWarning{-0.07};       ///< Drawdown warning threshold
    double maxVaR95{-0.03};                  ///< Max 1-day 95% VaR as fraction of NAV
    bool   resetDailyOnSessionStart{true};   ///< Reset daily P&L at session open

    [[nodiscard]] bool isHardBreach(double dailyPnl, double drawdownPct) const noexcept {
        return dailyPnl <= maxDailyLoss ||
               drawdownPct <= maxDrawdownPct;
    }
    [[nodiscard]] bool isSoftWarning(double dailyPnl, double drawdownPct) const noexcept {
        return (dailyPnl   <= softDailyLossWarning && dailyPnl   > maxDailyLoss) ||
               (drawdownPct <= softDrawdownWarning  && drawdownPct > maxDrawdownPct);
    }
};

// ─────────────────────────────────────────────────────────────
// RiskLimits — complete risk configuration
// ─────────────────────────────────────────────────────────────

struct RiskLimits {
    OrderLimits     order;
    PortfolioLimits portfolio;
    SessionLimits   session;
    std::unordered_map<Symbol, SymbolLimits> perSymbol;

    /// Get symbol-specific limits (falls back to defaults if not configured).
    [[nodiscard]] SymbolLimits getSymbolLimits(const Symbol& sym) const {
        auto it = perSymbol.find(sym);
        if (it != perSymbol.end()) return it->second;
        // Return default limits with symbol name filled in
        SymbolLimits def;
        def.symbol = sym;
        return def;
    }

    /// Print all configured limits.
    [[nodiscard]] std::string toString() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "═══════════════ Risk Limits ═══════════════\n"
            << "Order:\n"
            << "  maxOrderQty      = " << order.maxOrderQty        << "\n"
            << "  maxOrderNotional = $" << order.maxOrderNotional   << "\n"
            << "  maxOrdersPerSec  = " << order.maxOrdersPerSecond  << "\n"
            << "  allowMarket      = " << order.allowMarketOrders   << "\n"
            << "  allowShort       = " << order.allowShortSelling   << "\n"
            << "Portfolio:\n"
            << "  maxGrossExposure = $" << portfolio.maxGrossExposure << "\n"
            << "  maxNetExposure   = $" << portfolio.maxNetExposure   << "\n"
            << "  maxGrossLeverage = "  << portfolio.maxGrossLeverage << "x\n"
            << "  maxConcentration = "  << (portfolio.maxConcentrationPct*100) << "%\n"
            << "Session:\n"
            << "  maxDailyLoss     = $" << session.maxDailyLoss      << "\n"
            << "  maxDrawdown      = "  << (session.maxDrawdownPct*100) << "%\n"
            << "  maxVaR95         = "  << (session.maxVaR95*100)    << "%\n"
            << "PerSymbol: " << perSymbol.size() << " configured\n"
            << "═══════════════════════════════════════════\n";
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────
// Pre-trade check result
// ─────────────────────────────────────────────────────────────

struct PreTradeResult {
    bool   approved{true};
    std::string rejectReason;

    static PreTradeResult ok() { return {true, ""}; }
    static PreTradeResult reject(std::string reason) {
        return {false, std::move(reason)};
    }
};

} // namespace qtl
