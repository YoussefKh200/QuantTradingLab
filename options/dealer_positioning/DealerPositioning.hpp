#pragma once
/**
 * @file options/dealer_positioning/DealerPositioning.hpp
 * @brief Dealer net-gamma positioning and charm/vanna exposure aggregation.
 *
 * Extends GEX with:
 *  - Vanna Exposure (VannEx): ∂delta/∂vol × OI — drives price when vol moves
 *  - Charm Exposure (CharmEx): ∂delta/∂t  × OI — drives price as expiry nears
 *  - Total dealer delta hedge requirement
 *
 * These second-order flows are significant near expiry and explain
 * daily directional pressure in the underlying from dealer hedging.
 */

#include "options/gamma_exposure/GammaExposure.hpp"

namespace qtl {

// ─────────────────────────────────────────────────────────────
// DealerHedgingFlow — estimated daily hedge requirement
// ─────────────────────────────────────────────────────────────

struct DealerHedgingFlow {
    double gammaHedge{0.0};   ///< Shares to buy/sell from gamma hedging (per 1% move)
    double vannaHedge{0.0};   ///< Shares from vanna hedging (per 1% vol move)
    double charmHedge{0.0};   ///< Shares from charm bleed (per day)
    double totalFlow{0.0};    ///< Net estimated daily flow ($)

    [[nodiscard]] std::string toString() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "DealerFlow: gamma=" << gammaHedge
            << " vanna=" << vannaHedge
            << " charm=" << charmHedge
            << " totalFlow=$" << totalFlow << "M";
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────
// DealerPositioningAnalyzer
// ─────────────────────────────────────────────────────────────

class DealerPositioningAnalyzer {
public:
    /**
     * @brief Compute full dealer positioning including second-order exposures.
     *
     * @param contracts    Options chain
     * @param spot         Current spot
     * @param riskFree     Risk-free rate
     * @param divYield     Dividend yield
     * @return GEXProfile with vanna/charm fields populated
     */
    [[nodiscard]] static GEXProfile analyze(
            const std::vector<OptionContract>& contracts,
            double spot,
            double riskFree = 0.05,
            double divYield = 0.0)
    {
        return GEXCalculator::compute(contracts, spot, riskFree, divYield);
    }

    /**
     * @brief Estimate daily dealer hedging flow.
     *
     * @param profile    Pre-computed GEX profile
     * @param spotMove   Hypothetical % spot move (e.g. 0.01 = 1%)
     * @param volMove    Hypothetical % vol move  (e.g. 0.01 = 1 vega point)
     */
    [[nodiscard]] static DealerHedgingFlow estimateFlow(
            const GEXProfile& profile,
            double spotMove = 0.01,
            double volMove  = 0.01)
    {
        DealerHedgingFlow flow;
        // Gamma hedge: GEX × spot_move
        flow.gammaHedge = profile.totalGEX * 1e9 / profile.spot * spotMove;
        // Vanna hedge: DEX change per vol move (approximate)
        flow.vannaHedge = profile.totalDEX * 1e6 * volMove;
        // Charm bleed: approximate daily delta decay
        flow.charmHedge = profile.totalDEX * 1e6 / 252.0;
        // Total flow in millions
        flow.totalFlow  = (flow.gammaHedge + flow.vannaHedge + flow.charmHedge)
                          / 1e6;
        return flow;
    }

    /**
     * @brief Identify the most significant gamma levels near spot.
     * Returns strikes within pctRange% of spot, sorted by |GEX|.
     */
    [[nodiscard]] static std::vector<std::pair<double,double>>
    significantLevels(const GEXProfile& profile,
                       double pctRange = 0.05) {
        std::vector<std::pair<double,double>> levels;
        double lo = profile.spot * (1.0 - pctRange);
        double hi = profile.spot * (1.0 + pctRange);
        for (size_t i = 0; i < profile.strikes.size(); ++i) {
            double k = profile.strikes[i];
            if (k >= lo && k <= hi) {
                levels.emplace_back(k, profile.gexPerStrike[i]);
            }
        }
        std::sort(levels.begin(), levels.end(),
                  [](auto& a, auto& b){
                      return std::abs(a.second) > std::abs(b.second);
                  });
        return levels;
    }
};

} // namespace qtl
