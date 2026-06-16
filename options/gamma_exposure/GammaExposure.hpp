#pragma once
/**
 * @file options/gamma_exposure/GammaExposure.hpp
 * @brief Institutional-grade dealer exposure analytics.
 *
 * Computes the full suite of dealer-hedging exposures across an options chain:
 *
 *   GEX  — Gamma Exposure   ∂²P/∂S² × OI × mult × S²/100
 *   DEX  — Delta Exposure   ∂P/∂S   × OI × mult × S
 *   VEX  — Vega Exposure    ∂P/∂σ   × OI × mult
 *   VannEx — Vanna Exposure ∂²P/∂S∂σ × OI × mult × S
 *   CharmEx— Charm Exposure ∂²P/∂S∂t × OI × mult × S
 *
 * Dealer sign convention (standard market-maker book assumption):
 *   Calls: dealers SHORT → positive GEX (sell into rallies / buy dips)
 *   Puts:  dealers SHORT → negative GEX (buy into rallies / sell dips)
 *
 * Key levels derived:
 *   Zero-Gamma Level  — spot where aggregate GEX = 0 (vol regime boundary)
 *   Gamma Wall        — highest positive GEX strike (resistance)
 *   Call Wall         — strike with highest call OI (likely resistance)
 *   Put Wall          — strike with highest put OI (likely support)
 *   Max Pain          — strike minimising total option intrinsic value
 *   Vol Trigger       — strike where VannEx flips sign
 *
 * References:
 *   - Squeezemetrics GEX paper (2017)
 *   - Cem Karsan dealer positioning framework
 *   - SpotGamma strike-based exposure methodology
 */

#include "options/blackscholes/BlackScholes.hpp"
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <stdexcept>
#include <map>
#include <functional>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// OptionContract — one row in the options chain
// ─────────────────────────────────────────────────────────────

struct OptionContract {
    double     strike{0.0};
    OptionType type{OptionType::Call};
    double     openInterest{0.0};     ///< OI in contracts
    double     impliedVol{0.0};       ///< Market IV for this strike
    double     daysToExpiry{30.0};
    double     multiplier{100.0};     ///< Shares per contract (100 for equity)
    double     volume{0.0};           ///< Daily volume (optional)
    double     marketPrice{0.0};      ///< Option market price (optional)

    // Computed by GEXCalculator::compute() — zero until computed
    double gamma{0.0};
    double delta{0.0};
    double vega{0.0};
    double vanna{0.0};
    double charm{0.0};
    double theta{0.0};

    // Exposure contributions
    double gexContrib{0.0};
    double dexContrib{0.0};
    double vexContrib{0.0};
    double vannexContrib{0.0};
    double charmexContrib{0.0};
};

// ─────────────────────────────────────────────────────────────
// StrikeProfile — all exposures at one strike across expiries
// ─────────────────────────────────────────────────────────────

struct StrikeProfile {
    double strike{0.0};
    double callOI{0.0},    putOI{0.0};
    double callGEX{0.0},   putGEX{0.0},   netGEX{0.0};
    double callDEX{0.0},   putDEX{0.0},   netDEX{0.0};
    double callVEX{0.0},   putVEX{0.0},   netVEX{0.0};
    double callVannEx{0.0},putVannEx{0.0},netVannEx{0.0};
    double callCharmEx{0.0},putCharmEx{0.0},netCharmEx{0.0};
};

// ─────────────────────────────────────────────────────────────
// GEXProfile — the complete dealer positioning snapshot
// ─────────────────────────────────────────────────────────────

struct GEXProfile {
    double spot{0.0};
    double asOfTime{0.0};  ///< Timestamp of snapshot

    // ── Aggregate portfolio exposures ──────────────────────────
    double totalGEX{0.0};          ///< Net GEX ($B)
    double totalCallGEX{0.0};      ///< Call-side GEX ($B)
    double totalPutGEX{0.0};       ///< Put-side GEX ($B)
    double totalDEX{0.0};          ///< Net delta exposure ($M)
    double totalVEX{0.0};          ///< Net vega exposure ($M)
    double totalVannEx{0.0};       ///< Net vanna exposure ($M)
    double totalCharmEx{0.0};      ///< Net charm exposure ($M per day)

    // ── Key levels ─────────────────────────────────────────────
    double zeroGammaLevel{0.0};    ///< GEX = 0 crossing (vol regime flip)
    double gammaWallStrike{0.0};   ///< Highest positive GEX strike
    double callWallStrike{0.0};    ///< Highest call OI strike
    double putWallStrike{0.0};     ///< Highest put OI strike
    double maxPainStrike{0.0};     ///< Max pain (holder loss minimisation)
    double volTriggerStrike{0.0};  ///< VannEx sign flip (vol accelerator)
    double highestGammaStrike{0.0};///< Absolute highest gamma strike

    // ── Per-strike profiles (sorted ascending by strike) ───────
    std::vector<StrikeProfile> strikeProfiles;

    // ── Regime indicators ──────────────────────────────────────
    [[nodiscard]] bool isPositiveGamma()    const noexcept { return totalGEX > 0; }
    [[nodiscard]] bool isNegativeGamma()    const noexcept { return totalGEX < 0; }
    [[nodiscard]] bool isAboveZeroGamma()   const noexcept {
        return zeroGammaLevel > 0 && spot > zeroGammaLevel;
    }
    [[nodiscard]] bool isBelowZeroGamma()   const noexcept {
        return zeroGammaLevel > 0 && spot < zeroGammaLevel;
    }

    // ── Convenience accessors ───────────────────────────────────
    [[nodiscard]] std::vector<double> strikes() const {
        std::vector<double> s;
        s.reserve(strikeProfiles.size());
        for (auto& p : strikeProfiles) s.push_back(p.strike);
        return s;
    }
    [[nodiscard]] std::vector<double> netGEXPerStrike() const {
        std::vector<double> v;
        v.reserve(strikeProfiles.size());
        for (auto& p : strikeProfiles) v.push_back(p.netGEX);
        return v;
    }
    [[nodiscard]] std::vector<double> netVannExPerStrike() const {
        std::vector<double> v;
        v.reserve(strikeProfiles.size());
        for (auto& p : strikeProfiles) v.push_back(p.netVannEx);
        return v;
    }
    [[nodiscard]] std::vector<double> netCharmExPerStrike() const {
        std::vector<double> v;
        v.reserve(strikeProfiles.size());
        for (auto& p : strikeProfiles) v.push_back(p.netCharmEx);
        return v;
    }

    // ── Summary string ─────────────────────────────────────────
    [[nodiscard]] std::string summary() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(3);
        oss << "══════════════════ GEX PROFILE ══════════════════\n"
            << "  Spot Price       : $" << spot             << "\n"
            << "  ─────────────── Aggregate Exposures ──────────\n"
            << "  Total GEX        : " << totalGEX  << " $B"
            << (isPositiveGamma() ? " [POSITIVE GAMMA]" : " [NEGATIVE GAMMA]") << "\n"
            << "  Call GEX         : " << totalCallGEX      << " $B\n"
            << "  Put  GEX         : " << totalPutGEX       << " $B\n"
            << "  Total DEX        : " << totalDEX          << " $M\n"
            << "  Total VEX        : " << totalVEX          << " $M\n"
            << "  Total VannEx     : " << totalVannEx       << " $M\n"
            << "  Total CharmEx    : " << totalCharmEx      << " $M/day\n"
            << "  ─────────────── Key Levels ────────────────────\n"
            << "  Zero-Gamma Level : $" << zeroGammaLevel   << "\n"
            << "  Gamma Wall       : $" << gammaWallStrike  << "\n"
            << "  Call Wall        : $" << callWallStrike   << "\n"
            << "  Put Wall         : $" << putWallStrike    << "\n"
            << "  Max Pain         : $" << maxPainStrike    << "\n"
            << "  Vol Trigger      : $" << volTriggerStrike << "\n"
            << "  ─────────────── Regime ────────────────────────\n"
            << "  Spot vs Zero-γ   : "
            << (isAboveZeroGamma() ? "ABOVE (pinning regime)"
                                    : isBelowZeroGamma() ? "BELOW (trending regime)"
                                                          : "AT ZERO-GAMMA") << "\n"
            << "═════════════════════════════════════════════════\n";
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────
// GEXCalculator — computes the full GEX profile
// ─────────────────────────────────────────────────────────────

class GEXCalculator {
public:
    /**
     * @brief Compute full GEX profile from an options chain.
     *
     * @param contracts   All option contracts (calls + puts, any expiry)
     * @param spot        Current spot price of the underlying
     * @param riskFree    Risk-free rate (annual, e.g. 0.05)
     * @param divYield    Continuous dividend yield (e.g. 0.015)
     */
    [[nodiscard]] static GEXProfile compute(
            const std::vector<OptionContract>& contracts,
            double spot,
            double riskFree  = 0.05,
            double divYield  = 0.0)
    {
        if (contracts.empty() || spot <= 0)
            throw std::invalid_argument("GEXCalculator: empty chain or zero spot");

        GEXProfile profile;
        profile.spot = spot;

        // Build sorted unique strike list
        std::map<double, StrikeProfile> byStrike;

        double maxCallOI = 0, maxPutOI = 0;
        double maxAbsGEX = 0;

        for (auto c : contracts) {
            // Ensure valid IV
            if (c.impliedVol <= 0) c.impliedVol = 0.25;
            double T = std::max(c.daysToExpiry / 365.0, 1.0 / 365.0);

            // Compute all Greeks via BSM
            BSMInput inp;
            inp.spot         = spot;
            inp.strike       = c.strike;
            inp.rate         = riskFree;
            inp.vol          = c.impliedVol;
            inp.timeToExpiry = T;
            inp.divYield     = divYield;
            inp.optionType   = c.type;

            auto bsm = BlackScholes::compute(inp);
            c.gamma  = bsm.gamma;
            c.delta  = bsm.delta;
            c.vega   = bsm.vega;
            c.vanna  = bsm.vanna;
            c.charm  = bsm.charm;
            c.theta  = bsm.theta;

            // ── Exposure calculations ─────────────────────────

            // GEX = gamma × OI × multiplier × spot² / 100
            double rawGEX = c.gamma * c.openInterest * c.multiplier
                            * spot * spot / 100.0;
            // DEX = delta × OI × multiplier × spot
            double rawDEX = c.delta * c.openInterest * c.multiplier * spot;
            // VEX = vega × OI × multiplier
            double rawVEX = c.vega * c.openInterest * c.multiplier;
            // VannEx = vanna × OI × multiplier × spot
            double rawVannEx = c.vanna * c.openInterest * c.multiplier * spot;
            // CharmEx = charm × OI × multiplier × spot (per day)
            double rawCharmEx = c.charm * c.openInterest * c.multiplier * spot;

            // Dealer sign convention: short calls, short puts
            // Calls → +GEX; Puts → -GEX
            double signedGEX = (c.type == OptionType::Call) ? rawGEX : -rawGEX;
            // DEX: calls positive, puts negative (dealers are short both)
            double signedDEX = (c.type == OptionType::Call) ? rawDEX : -rawDEX;
            // VEX same sign: short vol on both
            double signedVEX = rawVEX;
            // VannEx: calls positive, puts negative
            double signedVannEx = (c.type == OptionType::Call) ? rawVannEx : -rawVannEx;
            // CharmEx: calls positive, puts negative
            double signedCharmEx = (c.type == OptionType::Call) ? rawCharmEx : -rawCharmEx;

            c.gexContrib     = signedGEX;
            c.dexContrib     = signedDEX;
            c.vexContrib     = signedVEX;
            c.vannexContrib  = signedVannEx;
            c.charmexContrib = signedCharmEx;

            // Accumulate portfolio totals
            profile.totalGEX     += signedGEX;
            profile.totalDEX     += signedDEX;
            profile.totalVEX     += signedVEX;
            profile.totalVannEx  += signedVannEx;
            profile.totalCharmEx += signedCharmEx;
            if (c.type == OptionType::Call) profile.totalCallGEX += signedGEX;
            else                            profile.totalPutGEX  += signedGEX;

            // Per-strike accumulation
            auto& sp = byStrike[c.strike];
            sp.strike = c.strike;
            if (c.type == OptionType::Call) {
                sp.callOI     += c.openInterest;
                sp.callGEX    += signedGEX;
                sp.callDEX    += signedDEX;
                sp.callVEX    += signedVEX;
                sp.callVannEx += signedVannEx;
                sp.callCharmEx+= signedCharmEx;
            } else {
                sp.putOI      += c.openInterest;
                sp.putGEX     += signedGEX;
                sp.putDEX     += signedDEX;
                sp.putVEX     += signedVEX;
                sp.putVannEx  += signedVannEx;
                sp.putCharmEx += signedCharmEx;
            }

            // Key level tracking
            if (c.type == OptionType::Call && c.openInterest > maxCallOI) {
                maxCallOI = c.openInterest;
                profile.callWallStrike = c.strike;
            }
            if (c.type == OptionType::Put  && c.openInterest > maxPutOI) {
                maxPutOI = c.openInterest;
                profile.putWallStrike  = c.strike;
            }
        }

        // Compute net per strike and copy to vector
        for (auto& [k, sp] : byStrike) {
            sp.netGEX     = sp.callGEX     + sp.putGEX;
            sp.netDEX     = sp.callDEX     + sp.putDEX;
            sp.netVEX     = sp.callVEX     + sp.putVEX;
            sp.netVannEx  = sp.callVannEx  + sp.putVannEx;
            sp.netCharmEx = sp.callCharmEx + sp.putCharmEx;
            profile.strikeProfiles.push_back(sp);

            if (std::abs(sp.netGEX) > maxAbsGEX) {
                maxAbsGEX = std::abs(sp.netGEX);
                if (sp.netGEX > 0) profile.gammaWallStrike = k;
                profile.highestGammaStrike = k;
            }
        }

        // Scale to standard units (GEX→$B, DEX/VEX/VannEx/CharmEx→$M)
        const double kToBillions = 1e-9;
        const double kToMillions = 1e-6;
        profile.totalGEX      *= kToBillions;
        profile.totalCallGEX  *= kToBillions;
        profile.totalPutGEX   *= kToBillions;
        profile.totalDEX      *= kToMillions;
        profile.totalVEX      *= kToMillions;
        profile.totalVannEx   *= kToMillions;
        profile.totalCharmEx  *= kToMillions;

        // Scale per-strike GEX for chart (keep raw for zero-gamma finding)
        // Derived levels
        profile.zeroGammaLevel  = findZeroGamma(profile.strikeProfiles, spot);
        profile.maxPainStrike   = computeMaxPain(contracts);
        profile.volTriggerStrike= findVolTrigger(profile.strikeProfiles, spot);

        // Gamma wall: highest positive net GEX strike near spot
        double bestGEX = -1e18;
        for (auto& sp : profile.strikeProfiles) {
            if (sp.netGEX > bestGEX) {
                bestGEX = sp.netGEX;
                profile.gammaWallStrike = sp.strike;
            }
        }

        return profile;
    }

    // ── Exposure charting ─────────────────────────────────────

    /**
     * @brief Render an ASCII bar chart of GEX across strikes.
     *
     * @param profile    Computed GEX profile
     * @param nStrikes   Max strikes to show (0 = all)
     * @param width      Bar chart width in characters
     */
    [[nodiscard]] static std::string chartGEX(
            const GEXProfile& profile,
            size_t nStrikes = 20,
            int    width    = 40)
    {
        const auto& sps = profile.strikeProfiles;
        if (sps.empty()) return "No data\n";

        // Find range around spot
        std::vector<const StrikeProfile*> visible;
        for (auto& sp : sps) visible.push_back(&sp);

        // Sort by distance from spot, take nStrikes nearest
        std::sort(visible.begin(), visible.end(),
                  [&](const StrikeProfile* a, const StrikeProfile* b){
                      return std::abs(a->strike - profile.spot) <
                             std::abs(b->strike - profile.spot);
                  });
        if (nStrikes && visible.size() > nStrikes)
            visible.resize(nStrikes);

        // Re-sort by strike ascending
        std::sort(visible.begin(), visible.end(),
                  [](const StrikeProfile* a, const StrikeProfile* b){
                      return a->strike < b->strike;
                  });

        double maxAbs = 0;
        for (auto* sp : visible)
            maxAbs = std::max(maxAbs, std::abs(sp->netGEX));
        if (maxAbs == 0) maxAbs = 1;

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);

        // Header
        oss << "\n+─────────────── GEX Exposure Chart ───────────────+\n";
        oss << "| Spot: $" << std::setw(8) << profile.spot
            << "  Zero-γ: $" << std::setw(8) << profile.zeroGammaLevel << " |\n";
        oss << "+──────────+────────────────────────────────────────────+\n";
        oss << "| Strike   | GEX ($M)                                    |\n";
        oss << "+──────────+──────────────────────────────────────────────+\n";

        for (auto* sp : visible) {
            double gexM = sp->netGEX / 1e6;  // display in $M (raw)
            int barLen  = static_cast<int>(
                std::abs(sp->netGEX) / maxAbs * width);
            barLen = std::clamp(barLen, 0, width);

            std::string bar(static_cast<size_t>(barLen),
                            gexM >= 0 ? '+' : '-');
            std::string pad(static_cast<size_t>(width - barLen), ' ');

            // Mark key levels
            char marker = ' ';
            if (std::abs(sp->strike - profile.gammaWallStrike) < 0.01) marker = 'G';
            else if (std::abs(sp->strike - profile.callWallStrike) < 0.01) marker = 'C';
            else if (std::abs(sp->strike - profile.putWallStrike)  < 0.01) marker = 'P';
            else if (std::abs(sp->strike - profile.spot) < 0.01)           marker = '*';

            oss << "| " << std::setw(7) << sp->strike << marker << " |"
                << (gexM >= 0 ? "+" : "-")
                << bar << pad << "|\n";
        }

        oss << "+──────────+──────────────────────────────────────────────+\n";
        oss << "| Legend: G=GammaWall C=CallWall P=PutWall *=Spot         |\n";
        oss << "+─────────────────────────────────────────────────────────+\n";
        return oss.str();
    }

    /**
     * @brief Render ASCII chart of Vanna Exposure (VannEx) across strikes.
     */
    [[nodiscard]] static std::string chartVannEx(
            const GEXProfile& profile,
            size_t nStrikes = 20,
            int    width    = 35)
    {
        return chartExposure(profile, nStrikes, width,
                              "VannEx Exposure Chart",
                              [](const StrikeProfile& sp){ return sp.netVannEx; });
    }

    /**
     * @brief Render ASCII chart of Charm Exposure (CharmEx) across strikes.
     */
    [[nodiscard]] static std::string chartCharmEx(
            const GEXProfile& profile,
            size_t nStrikes = 20,
            int    width    = 35)
    {
        return chartExposure(profile, nStrikes, width,
                              "CharmEx Exposure Chart",
                              [](const StrikeProfile& sp){ return sp.netCharmEx; });
    }

private:
    // Generic exposure chart renderer
    static std::string chartExposure(
            const GEXProfile& profile,
            size_t nStrikes, int width,
            const std::string& title,
            std::function<double(const StrikeProfile&)> getValue)
    {
        const auto& sps = profile.strikeProfiles;
        if (sps.empty()) return "No data\n";

        std::vector<const StrikeProfile*> visible;
        for (auto& sp : sps) visible.push_back(&sp);

        std::sort(visible.begin(), visible.end(),
                  [&](const StrikeProfile* a, const StrikeProfile* b){
                      return std::abs(a->strike - profile.spot) <
                             std::abs(b->strike - profile.spot);
                  });
        if (nStrikes && visible.size() > nStrikes) visible.resize(nStrikes);
        std::sort(visible.begin(), visible.end(),
                  [](const StrikeProfile* a, const StrikeProfile* b){
                      return a->strike < b->strike;
                  });

        double maxAbs = 0;
        for (auto* sp : visible) maxAbs = std::max(maxAbs, std::abs(getValue(*sp)));
        if (maxAbs == 0) maxAbs = 1;

        std::ostringstream oss;
        oss << "\n+─── " << title << " ───+\n";
        for (auto* sp : visible) {
            double val  = getValue(*sp);
            int barLen  = static_cast<int>(std::abs(val) / maxAbs * width);
            barLen = std::clamp(barLen, 0, width);
            std::string bar(static_cast<size_t>(barLen), val >= 0 ? '+' : '-');
            oss << std::setw(7) << std::fixed << std::setprecision(1) << sp->strike
                << " | " << (val >= 0 ? "+" : " ")
                << std::setw(width) << std::left << bar << "|\n";
        }
        oss << "+─────────────────────────────────────────────────+\n";
        return oss.str();
    }

    static double findZeroGamma(const std::vector<StrikeProfile>& sps, double spot) {
        if (sps.size() < 2) return spot;
        for (size_t i = 1; i < sps.size(); ++i) {
            if (sps[i-1].netGEX * sps[i].netGEX < 0) {
                // Linear interpolation between sps[i-1] and sps[i]
                double g0 = sps[i-1].netGEX, g1 = sps[i].netGEX;
                double k0 = sps[i-1].strike, k1 = sps[i].strike;
                double w  = -g0 / (g1 - g0);
                return k0 + w * (k1 - k0);
            }
        }
        return spot; // No crossing found
    }

    static double findVolTrigger(const std::vector<StrikeProfile>& sps, double spot) {
        if (sps.size() < 2) return spot;
        for (size_t i = 1; i < sps.size(); ++i) {
            if (sps[i-1].netVannEx * sps[i].netVannEx < 0) {
                double v0 = sps[i-1].netVannEx, v1 = sps[i].netVannEx;
                double k0 = sps[i-1].strike,    k1 = sps[i].strike;
                double w  = -v0 / (v1 - v0);
                return k0 + w * (k1 - k0);
            }
        }
        return spot;
    }

    static double computeMaxPain(const std::vector<OptionContract>& contracts) {
        // Collect unique strikes
        std::vector<double> strikes;
        for (auto& c : contracts) {
            if (std::find(strikes.begin(), strikes.end(), c.strike) == strikes.end())
                strikes.push_back(c.strike);
        }
        if (strikes.empty()) return 0.0;
        std::sort(strikes.begin(), strikes.end());

        double minPain = 1e18, mpStrike = strikes[0];
        for (double ks : strikes) {
            double pain = 0;
            for (auto& c : contracts) {
                double intr = (c.type == OptionType::Call)
                    ? std::max(0.0, ks - c.strike)
                    : std::max(0.0, c.strike - ks);
                pain += c.openInterest * intr;
            }
            if (pain < minPain) { minPain = pain; mpStrike = ks; }
        }
        return mpStrike;
    }
};

// ─────────────────────────────────────────────────────────────
// Convenience: build a realistic options chain for testing/demo
// ─────────────────────────────────────────────────────────────

/**
 * @brief Generate a synthetic options chain with vol smile.
 *
 * @param spot      Current spot price
 * @param nStrikes  Number of strikes each side of ATM
 * @param strikeStep Strike spacing
 * @param atmVol    ATM implied volatility
 * @param skew      Vol skew per strike (put skew positive)
 * @param dte       Days to expiry
 * @param oiBase    Base open interest (ATM)
 */
[[nodiscard]] inline std::vector<OptionContract>
buildSyntheticChain(double spot,
                     int    nStrikes  = 10,
                     double strikeStep= 5.0,
                     double atmVol    = 0.20,
                     double skew      = 0.003,
                     double dte       = 30.0,
                     double oiBase    = 10000.0)
{
    std::vector<OptionContract> chain;

    for (int i = -nStrikes; i <= nStrikes; ++i) {
        double K   = std::round(spot + i * strikeStep);
        double moneyness = (K - spot) / spot;

        // Vol smile: higher IV for OTM puts and calls
        double iv  = atmVol + std::abs(moneyness) * skew * 100.0;
        // Skew: OTM puts have higher IV than OTM calls
        if (K < spot) iv += skew * 5.0;
        iv = std::max(0.05, iv);

        // OI: higher near ATM, tapers off
        double oiScale = std::exp(-0.5 * moneyness * moneyness * 400.0);
        double oi = oiBase * oiScale;

        OptionContract call, put;
        call.strike = put.strike = K;
        call.type   = OptionType::Call;
        put.type    = OptionType::Put;
        call.impliedVol = put.impliedVol = iv;
        call.openInterest = oi * (K >= spot ? 1.2 : 0.8);  // call-heavy above
        put.openInterest  = oi * (K <= spot ? 1.2 : 0.8);  // put-heavy below
        call.daysToExpiry = put.daysToExpiry = dte;
        call.multiplier   = put.multiplier   = 100.0;

        chain.push_back(call);
        chain.push_back(put);
    }
    return chain;
}

} // namespace qtl
