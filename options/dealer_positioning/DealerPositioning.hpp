#pragma once
/**
 * @file options/dealer_positioning/DealerPositioning.hpp
 * @brief Full dealer positioning engine — hedging flows, regime detection, reports.
 *
 * Builds on GEXCalculator to provide:
 *  1. Daily hedging flow estimates (gamma, vanna, charm)
 *  2. Vol regime classification (positive/negative gamma)
 *  3. Spot vs key level distance analysis
 *  4. Significant strike ranking
 *  5. Full text report generation
 *  6. Multi-expiry roll-up (aggregate across all DTEs)
 *
 * Hedging flow model
 * ──────────────────
 * When spot moves 1%, dealers must hedge their gamma exposure:
 *   ΔShares_gamma = GEX × spot × 0.01    (shares to trade per 1% move)
 *
 * When IV moves 1 vol point, dealers must hedge vanna:
 *   ΔShares_vanna = VannEx × 0.01        (shares per 1% IV change)
 *
 * Charm decay creates daily delta drift:
 *   ΔShares_charm = CharmEx / 252        (shares per calendar day)
 *
 * These flows predict intraday market direction:
 *   - Positive gamma regime: hedging flows oppose spot moves (pinning)
 *   - Negative gamma regime: hedging flows amplify spot moves (trending)
 */

#include "options/gamma_exposure/GammaExposure.hpp"
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cmath>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// VolRegime — macro regime classification
// ─────────────────────────────────────────────────────────────

enum class VolRegime : uint8_t {
    PositiveGamma,    ///< Dealers long gamma → pinning / mean-reversion
    NegativeGamma,    ///< Dealers short gamma → trending / gap risk
    NeutralGamma,     ///< Near zero-gamma level (transition zone ±5%)
};

inline std::string volRegimeName(VolRegime r) {
    switch (r) {
        case VolRegime::PositiveGamma: return "POSITIVE GAMMA (Pinning)";
        case VolRegime::NegativeGamma: return "NEGATIVE GAMMA (Trending)";
        case VolRegime::NeutralGamma:  return "NEUTRAL (Transition)";
    }
    return "UNKNOWN";
}

// ─────────────────────────────────────────────────────────────
// DealerHedgingFlow — estimated daily hedging demand
// ─────────────────────────────────────────────────────────────

struct DealerHedgingFlow {
    // Per-move flows
    double gammaFlowPer1pctMove{0.0};   ///< Shares dealers trade per 1% spot move
    double vannaFlowPer1ptVol{0.0};     ///< Shares per 1 vol point change
    double charmFlowPerDay{0.0};        ///< Shares from daily theta decay

    // $ equivalents (at spot)
    double gammaDollarFlow{0.0};        ///< $ per 1% spot move
    double vannaDollarFlow{0.0};        ///< $ per 1 vol point
    double charmDollarFlow{0.0};        ///< $ daily charm bleed

    // Net directional bias
    double netDailyBias{0.0};           ///< Net $ daily flow estimate (charm + partial vanna)
    bool   buyBias{false};              ///< True if net flow is buying pressure

    [[nodiscard]] std::string toString() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "DealerFlow:\n"
            << "  Gamma  : " << gammaFlowPer1pctMove << " shares/$M per 1% spot move\n"
            << "  Vanna  : " << vannaFlowPer1ptVol   << " shares/$M per 1 vol point\n"
            << "  Charm  : " << charmFlowPerDay       << " shares/$M per day\n"
            << "  Net Bias: $" << netDailyBias << "M/day "
            << (buyBias ? "(BUY)" : "(SELL)") << "\n";
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────
// KeyLevelDistance — spot distance from each key level
// ─────────────────────────────────────────────────────────────

struct KeyLevelDistance {
    double spot{0.0};
    double distToZeroGamma{0.0};    ///< % distance to zero-gamma level
    double distToGammaWall{0.0};    ///< % distance to gamma wall
    double distToCallWall{0.0};     ///< % distance to call wall
    double distToPutWall{0.0};      ///< % distance to put wall
    double distToMaxPain{0.0};      ///< % distance to max pain
    double distToVolTrigger{0.0};   ///< % distance to vol trigger

    static double pctDist(double spot, double level) noexcept {
        return level > 0 ? (level - spot) / spot * 100.0 : 0.0;
    }

    [[nodiscard]] std::string toString() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "Key Level Distances (from spot $" << spot << "):\n"
            << "  Zero-Gamma  : " << distToZeroGamma  << "%\n"
            << "  Gamma Wall  : " << distToGammaWall  << "%\n"
            << "  Call Wall   : " << distToCallWall   << "%\n"
            << "  Put Wall    : " << distToPutWall    << "%\n"
            << "  Max Pain    : " << distToMaxPain    << "%\n"
            << "  Vol Trigger : " << distToVolTrigger << "%\n";
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────
// DealerPositioningAnalyzer
// ─────────────────────────────────────────────────────────────

class DealerPositioningAnalyzer {
public:
    /**
     * @brief Analyze a full options chain and return the GEX profile.
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
     * @brief Classify the current vol regime.
     *
     * @param profile   Computed GEX profile
     * @param neutralBandPct  % band around zero-gamma for "neutral" zone
     */
    [[nodiscard]] static VolRegime classifyRegime(
            const GEXProfile& profile,
            double neutralBandPct = 0.02) noexcept
    {
        double spot = profile.spot;
        double zg   = profile.zeroGammaLevel;

        if (zg > 0) {
            double distPct = std::abs(spot - zg) / spot;
            if (distPct < neutralBandPct) return VolRegime::NeutralGamma;
        }

        return profile.isPositiveGamma()
                   ? VolRegime::PositiveGamma
                   : VolRegime::NegativeGamma;
    }

    /**
     * @brief Estimate daily dealer hedging flows.
     *
     * @param profile    GEX profile
     * @param spotMovePct 1-day spot move assumption (default 1%)
     * @param volMovePts  1-day vol change assumption (default 1 vol point = 0.01)
     */
    [[nodiscard]] static DealerHedgingFlow estimateFlow(
            const GEXProfile& profile,
            double spotMovePct = 0.01,
            double volMovePts  = 0.01) noexcept
    {
        DealerHedgingFlow flow;
        double spot = profile.spot;
        if (spot <= 0) return flow;

        // GEX is in $B — convert back to raw for share calculation
        double rawGEX   = profile.totalGEX   * 1e9;
        double rawVannEx = profile.totalVannEx * 1e6;
        double rawCharmEx = profile.totalCharmEx * 1e6;

        // Gamma: dealers must trade GEX × spot_move shares per 1% spot move
        flow.gammaFlowPer1pctMove = rawGEX / spot * spotMovePct;
        flow.gammaDollarFlow      = flow.gammaFlowPer1pctMove * spot / 1e6;

        // Vanna: per vol point move
        flow.vannaFlowPer1ptVol   = rawVannEx / spot * volMovePts;
        flow.vannaDollarFlow      = flow.vannaFlowPer1ptVol * spot / 1e6;

        // Charm: daily delta bleed
        flow.charmFlowPerDay      = rawCharmEx / 252.0;
        flow.charmDollarFlow      = flow.charmFlowPerDay * spot / 1e6;

        // Net daily bias (charm + partial vanna from typical daily vol move)
        flow.netDailyBias = flow.charmDollarFlow + flow.vannaDollarFlow;
        flow.buyBias      = flow.netDailyBias > 0;

        return flow;
    }

    /**
     * @brief Compute distance from spot to all key levels.
     */
    [[nodiscard]] static KeyLevelDistance keyLevelDistances(
            const GEXProfile& profile) noexcept
    {
        KeyLevelDistance kld;
        kld.spot             = profile.spot;
        kld.distToZeroGamma  = KeyLevelDistance::pctDist(profile.spot, profile.zeroGammaLevel);
        kld.distToGammaWall  = KeyLevelDistance::pctDist(profile.spot, profile.gammaWallStrike);
        kld.distToCallWall   = KeyLevelDistance::pctDist(profile.spot, profile.callWallStrike);
        kld.distToPutWall    = KeyLevelDistance::pctDist(profile.spot, profile.putWallStrike);
        kld.distToMaxPain    = KeyLevelDistance::pctDist(profile.spot, profile.maxPainStrike);
        kld.distToVolTrigger = KeyLevelDistance::pctDist(profile.spot, profile.volTriggerStrike);
        return kld;
    }

    /**
     * @brief Return strikes ranked by absolute GEX contribution.
     * @param profile   GEX profile
     * @param topN      Number of strikes to return
     * @param pctRange  Only include strikes within this % of spot (0 = all)
     */
    [[nodiscard]] static std::vector<StrikeProfile>
    significantStrikes(const GEXProfile& profile,
                        size_t topN    = 10,
                        double pctRange= 0.10)
    {
        std::vector<const StrikeProfile*> candidates;
        for (auto& sp : profile.strikeProfiles) {
            if (pctRange > 0) {
                double dist = std::abs(sp.strike - profile.spot) / profile.spot;
                if (dist > pctRange) continue;
            }
            candidates.push_back(&sp);
        }

        std::sort(candidates.begin(), candidates.end(),
                  [](const StrikeProfile* a, const StrikeProfile* b){
                      return std::abs(a->netGEX) > std::abs(b->netGEX);
                  });

        if (topN && candidates.size() > topN) candidates.resize(topN);

        std::vector<StrikeProfile> result;
        for (auto* sp : candidates) result.push_back(*sp);
        return result;
    }

    /**
     * @brief Generate a full text positioning report.
     */
    [[nodiscard]] static std::string generateReport(
            const GEXProfile& profile,
            const std::string& underlying = "SPX")
    {
        std::ostringstream rpt;
        auto regime = classifyRegime(profile);
        auto flow   = estimateFlow(profile);
        auto kld    = keyLevelDistances(profile);
        auto sigStr = significantStrikes(profile, 5, 0.05);

        rpt << std::fixed << std::setprecision(2);
        rpt << "╔══════════════════════════════════════════════════════╗\n"
            << "║     DEALER POSITIONING REPORT — " << underlying
            << std::string(21 - underlying.size(), ' ') << "║\n"
            << "╠══════════════════════════════════════════════════════╣\n";

        // Spot & regime
        rpt << "║  Spot Price    : $" << std::setw(10) << profile.spot
            << "                         ║\n"
            << "║  Vol Regime    : " << std::setw(33) << std::left
            << volRegimeName(regime) << " ║\n" << std::right;

        // GEX summary
        rpt << "╠══════════════════════════════════════════════════════╣\n"
            << "║  EXPOSURE SUMMARY                                    ║\n"
            << "║  Net GEX       : " << std::setw(10) << profile.totalGEX    << " $B"
            << "                          ║\n"
            << "║  Call GEX      : " << std::setw(10) << profile.totalCallGEX << " $B"
            << "                          ║\n"
            << "║  Put  GEX      : " << std::setw(10) << profile.totalPutGEX  << " $B"
            << "                          ║\n"
            << "║  Net DEX       : " << std::setw(10) << profile.totalDEX     << " $M"
            << "                          ║\n"
            << "║  Net VEX       : " << std::setw(10) << profile.totalVEX     << " $M"
            << "                          ║\n"
            << "║  VannEx        : " << std::setw(10) << profile.totalVannEx  << " $M"
            << "                          ║\n"
            << "║  CharmEx       : " << std::setw(10) << profile.totalCharmEx << " $M/day"
            << "                      ║\n";

        // Key levels
        rpt << "╠══════════════════════════════════════════════════════╣\n"
            << "║  KEY LEVELS                                          ║\n"
            << "║  Zero-Gamma    : $" << std::setw(8) << profile.zeroGammaLevel
            << "  (" << std::setw(6) << kld.distToZeroGamma << "% from spot)  ║\n"
            << "║  Gamma Wall    : $" << std::setw(8) << profile.gammaWallStrike
            << "  (" << std::setw(6) << kld.distToGammaWall << "% from spot)  ║\n"
            << "║  Call Wall     : $" << std::setw(8) << profile.callWallStrike
            << "  (" << std::setw(6) << kld.distToCallWall  << "% from spot)  ║\n"
            << "║  Put Wall      : $" << std::setw(8) << profile.putWallStrike
            << "  (" << std::setw(6) << kld.distToPutWall   << "% from spot)  ║\n"
            << "║  Max Pain      : $" << std::setw(8) << profile.maxPainStrike
            << "  (" << std::setw(6) << kld.distToMaxPain   << "% from spot)  ║\n"
            << "║  Vol Trigger   : $" << std::setw(8) << profile.volTriggerStrike
            << "  (" << std::setw(6) << kld.distToVolTrigger<< "% from spot)  ║\n";

        // Hedging flows
        rpt << "╠══════════════════════════════════════════════════════╣\n"
            << "║  HEDGING FLOWS                                       ║\n"
            << "║  Gamma Flow    : $" << std::setw(8) << flow.gammaDollarFlow
            << " M per 1% spot move       ║\n"
            << "║  Vanna Flow    : $" << std::setw(8) << flow.vannaDollarFlow
            << " M per 1 vol pt           ║\n"
            << "║  Charm Flow    : $" << std::setw(8) << flow.charmDollarFlow
            << " M per day                ║\n"
            << "║  Net Daily     : $" << std::setw(8) << flow.netDailyBias
            << " M " << (flow.buyBias ? "(BUY)" : "(SELL)") << "              ║\n";

        // Significant strikes
        if (!sigStr.empty()) {
            rpt << "╠══════════════════════════════════════════════════════╣\n"
                << "║  TOP GAMMA STRIKES (within 5% of spot)               ║\n";
            for (auto& sp : sigStr) {
                rpt << "║    K=$" << std::setw(7) << sp.strike
                    << "  GEX=" << std::setw(10) << sp.netGEX / 1e6 << " $M"
                    << "  OI(C/P)=" << std::setw(6) << static_cast<int>(sp.callOI)
                    << "/" << std::setw(6) << static_cast<int>(sp.putOI) << " ║\n";
            }
        }

        rpt << "╚══════════════════════════════════════════════════════╝\n";

        // Append GEX chart
        rpt << GEXCalculator::chartGEX(profile, 15, 30);

        return rpt.str();
    }
};

} // namespace qtl
