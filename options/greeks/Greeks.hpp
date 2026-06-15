#pragma once
/**
 * @file options/greeks/Greeks.hpp
 * @brief Convenience wrappers and Greek aggregation for options portfolios.
 *
 * Provides:
 *  - Named Greek accessors (thin wrappers over BSMResult)
 *  - GreekSummary: aggregate Greeks over a portfolio of options
 *  - Dollar Greeks: Greeks scaled by notional for risk reporting
 *  - Greek sensitivities: how do Greeks change with scenario shocks
 */

#include "options/blackscholes/BlackScholes.hpp"
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <numeric>
#include <cmath>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// OptionPosition — one line in an options portfolio
// ─────────────────────────────────────────────────────────────

struct OptionPosition {
    Symbol      symbol;
    double      strike{0.0};
    double      expiry{0.0};     ///< Time to expiry in years
    OptionType  optionType{OptionType::Call};
    double      quantity{0.0};   ///< Signed: + long, - short; in contracts
    double      multiplier{100.0}; ///< Shares per contract (100 for US equities)
    double      impliedVol{0.0};
    double      spot{0.0};       ///< Last underlying price (for Greeks calc)
    double      rate{0.0};
    double      divYield{0.0};

    [[nodiscard]] BSMInput toBSM() const noexcept {
        return {spot, strike, rate, impliedVol, expiry,
                divYield, optionType};
    }

    [[nodiscard]] BSMResult greeks() const noexcept {
        return BlackScholes::compute(toBSM());
    }
};

// ─────────────────────────────────────────────────────────────
// PortfolioGreeks — aggregate risk across option positions
// ─────────────────────────────────────────────────────────────

struct PortfolioGreeks {
    // Net Greeks (signed, summed across all positions)
    double netDelta{0.0};     ///< ΔΣ = Σ(qty * multiplier * delta)
    double netGamma{0.0};     ///< Dollar gamma: Σ(qty * multiplier * gamma * S)
    double netTheta{0.0};     ///< Daily theta decay ($): Σ(qty * multiplier * theta)
    double netVega{0.0};      ///< Vega ($per 1% vol): Σ(qty * multiplier * vega)
    double netRho{0.0};       ///< Rho ($per 1%): Σ(qty * multiplier * rho)
    double netVanna{0.0};
    double netCharm{0.0};
    double netVomma{0.0};

    // Position metrics
    double totalNotional{0.0}; ///< Σ |qty * multiplier * spot|
    double totalPremium{0.0};  ///< Σ qty * multiplier * price
    int    positionCount{0};

    [[nodiscard]] std::string toString() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4);
        oss << "═══ Portfolio Greeks ════════════════════\n"
            << "  Positions    : " << positionCount    << "\n"
            << "  Net Notional : $" << totalNotional   << "\n"
            << "  Net Premium  : $" << totalPremium    << "\n"
            << "  Net Delta    : "  << netDelta        << "\n"
            << "  Net Gamma    : $" << netGamma        << "\n"
            << "  Net Theta/day: $" << netTheta        << "\n"
            << "  Net Vega/1%  : $" << netVega         << "\n"
            << "  Net Rho/1%   : $" << netRho          << "\n"
            << "  Net Vanna    : "  << netVanna        << "\n"
            << "  Net Charm/day: "  << netCharm        << "\n"
            << "  Net Vomma    : "  << netVomma        << "\n"
            << "════════════════════════════════════════\n";
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────
// GreeksCalculator — portfolio-level Greek aggregation
// ─────────────────────────────────────────────────────────────

class GreeksCalculator {
public:

    /**
     * @brief Aggregate Greeks across a portfolio of option positions.
     */
    [[nodiscard]] static PortfolioGreeks aggregate(
            const std::vector<OptionPosition>& positions) {
        PortfolioGreeks pg;
        pg.positionCount = static_cast<int>(positions.size());

        for (const auto& pos : positions) {
            auto r = pos.greeks();
            double bsPrice = r.price;
            double scale   = pos.quantity * pos.multiplier;

            pg.totalPremium  += scale * bsPrice;
            pg.totalNotional += std::abs(scale) * pos.spot;

            pg.netDelta  += scale * r.delta;
            pg.netGamma  += scale * r.gamma * pos.spot; // dollar gamma
            pg.netTheta  += scale * r.theta;
            pg.netVega   += scale * r.vega;
            pg.netRho    += scale * r.rho;
            pg.netVanna  += scale * r.vanna;
            pg.netCharm  += scale * r.charm;
            pg.netVomma  += scale * r.vomma;
        }
        return pg;
    }

    /**
     * @brief Compute P&L for a spot price shock of `dS`.
     * First-order approximation: PnL ≈ delta*dS + 0.5*gamma*dS²
     */
    [[nodiscard]] static double spotPnl(const PortfolioGreeks& pg,
                                         double dS) noexcept {
        return pg.netDelta * dS + 0.5 * pg.netGamma / pg.totalNotional * dS * dS;
    }

    /**
     * @brief Compute P&L for a vol shock of `dVol` (in decimal, e.g. 0.01 = 1%).
     * First-order: PnL ≈ vega * dVol * 100
     * (vega is per 1% vol move, dVol is absolute)
     */
    [[nodiscard]] static double volPnl(const PortfolioGreeks& pg,
                                        double dVol) noexcept {
        return pg.netVega * dVol * 100.0;
    }

    /**
     * @brief Scenario P&L grid: spot shock × vol shock.
     * Returns a vector<vector<double>> of size (nSpot × nVol).
     */
    [[nodiscard]] static std::vector<std::vector<double>>
    scenarioGrid(const std::vector<OptionPosition>& positions,
                  const std::vector<double>& spotShocks,
                  const std::vector<double>& volShocks) {
        size_t ns = spotShocks.size();
        size_t nv = volShocks.size();
        std::vector<std::vector<double>> grid(ns, std::vector<double>(nv, 0.0));

        // Base portfolio value
        double baseValue = 0.0;
        for (auto& pos : positions) {
            auto r = BlackScholes::compute(pos.toBSM());
            baseValue += pos.quantity * pos.multiplier * r.price;
        }

        for (size_t i = 0; i < ns; ++i) {
            for (size_t j = 0; j < nv; ++j) {
                double pnl = 0.0;
                for (auto& pos : positions) {
                    BSMInput shocked = pos.toBSM();
                    shocked.spot *= (1.0 + spotShocks[i]);
                    shocked.vol  += volShocks[j];
                    shocked.vol   = std::max(shocked.vol, 0.001);
                    auto r = BlackScholes::compute(shocked);
                    double shockedVal = pos.quantity * pos.multiplier * r.price;

                    BSMInput base = pos.toBSM();
                    auto rb = BlackScholes::compute(base);
                    double baseVal = pos.quantity * pos.multiplier * rb.price;
                    pnl += shockedVal - baseVal;
                }
                grid[i][j] = pnl;
            }
        }
        return grid;
    }
};

} // namespace qtl
