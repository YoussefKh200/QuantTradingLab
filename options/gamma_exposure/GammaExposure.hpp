#pragma once
/**
 * @file options/gamma_exposure/GammaExposure.hpp
 * @brief Gamma Exposure (GEX), Delta Exposure (DEX), Vega Exposure (VEX) calculator.
 *
 * Dealer GEX measures the aggregate gamma exposure of market makers
 * across all outstanding options contracts.  It predicts:
 *   - Positive GEX: dealers are long gamma → they sell into rallies and
 *     buy dips → price tends to mean-revert (low realised vol).
 *   - Negative GEX: dealers are short gamma → they buy into rallies and
 *     sell dips → price can trend / gap (high realised vol).
 *
 * Calculation per strike:
 *   GEX_strike = gamma × open_interest × contract_multiplier × spot²/100
 *   (multiplied by sign: calls add +GEX if dealers are short,
 *    puts add -GEX if dealers are short)
 *
 * Simplified assumption used here (standard market):
 *   Dealers are short calls and long puts (retail is long calls, short puts).
 *   GEX = Σ(call_OI × call_gamma - put_OI × put_gamma) × spot² × mult / 100
 *
 * Key levels derived from the GEX profile:
 *   - Zero-gamma level : spot where GEX changes sign (vol regime flip)
 *   - Gamma wall       : strike with highest positive GEX (strong resistance)
 *   - Call wall        : highest-OI call strike
 *   - Put wall         : highest-OI put strike
 *   - Max pain         : strike minimising total option value at expiry
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

namespace qtl {

// ─────────────────────────────────────────────────────────────
// OptionContract — one row of the options chain
// ─────────────────────────────────────────────────────────────

struct OptionContract {
    double     strike{0.0};
    OptionType type{OptionType::Call};
    double     openInterest{0.0};    ///< OI in contracts
    double     impliedVol{0.0};      ///< Market IV for this strike
    double     daysToExpiry{30.0};
    double     multiplier{100.0};    ///< Contracts multiplier (100 for equity options)

    // Computed fields (filled by GEXCalculator::compute())
    double     gamma{0.0};
    double     delta{0.0};
    double     vega{0.0};
    double     gexContribution{0.0};  ///< GEX contribution at this strike
    double     dexContribution{0.0};  ///< DEX contribution at this strike
    double     vexContribution{0.0};  ///< VEX contribution at this strike
};

// ─────────────────────────────────────────────────────────────
// GEXProfile — full gamma exposure across all strikes
// ─────────────────────────────────────────────────────────────

struct GEXProfile {
    double spot{0.0};

    // Aggregate exposures
    double totalGEX{0.0};          ///< Net GEX across all strikes/expiries
    double totalCallGEX{0.0};
    double totalPutGEX{0.0};
    double totalDEX{0.0};          ///< Net delta exposure
    double totalVEX{0.0};          ///< Net vega exposure

    // Key levels
    double zeroGammaLevel{0.0};    ///< Strike where GEX crosses zero
    double gammaWallStrike{0.0};   ///< Strike with highest +GEX (resistance)
    double callWallStrike{0.0};    ///< Strike with highest call OI
    double putWallStrike{0.0};     ///< Strike with highest put OI
    double maxPainStrike{0.0};     ///< Max pain strike

    // Per-strike GEX data (for charting)
    std::vector<double> strikes;
    std::vector<double> gexPerStrike;   ///< Net GEX at each strike
    std::vector<double> callGex;
    std::vector<double> putGex;

    [[nodiscard]] bool isPositiveGamma() const noexcept {
        return totalGEX > 0;
    }

    [[nodiscard]] std::string summary() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "══════════ GEX Profile ══════════\n"
            << "  Spot            : $" << spot           << "\n"
            << "  Total GEX       : $" << totalGEX       << "B\n"
            << "  Call GEX        : $" << totalCallGEX   << "B\n"
            << "  Put  GEX        : $" << totalPutGEX    << "B\n"
            << "  Total DEX       : $" << totalDEX       << "M\n"
            << "  Total VEX       : $" << totalVEX       << "M\n"
            << "  Regime          : "
            << (isPositiveGamma() ? "POSITIVE GAMMA (pinning)" : "NEGATIVE GAMMA (trending)") << "\n"
            << "  Zero-Gamma Level: $" << zeroGammaLevel << "\n"
            << "  Gamma Wall      : $" << gammaWallStrike<< "\n"
            << "  Call Wall       : $" << callWallStrike << "\n"
            << "  Put Wall        : $" << putWallStrike  << "\n"
            << "  Max Pain        : $" << maxPainStrike  << "\n"
            << "═════════════════════════════════\n";
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────
// GEXCalculator
// ─────────────────────────────────────────────────────────────

class GEXCalculator {
public:
    /**
     * @brief Compute the full GEX profile from an options chain.
     *
     * @param contracts   Vector of all option contracts (calls + puts)
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
            throw std::invalid_argument("GEXCalculator: empty contracts or zero spot");

        GEXProfile profile;
        profile.spot = spot;

        // Group by strike for per-strike GEX
        std::vector<double> uniqueStrikes;
        for (auto& c : contracts) {
            if (std::find(uniqueStrikes.begin(), uniqueStrikes.end(),
                          c.strike) == uniqueStrikes.end()) {
                uniqueStrikes.push_back(c.strike);
            }
        }
        std::sort(uniqueStrikes.begin(), uniqueStrikes.end());

        profile.strikes.resize(uniqueStrikes.size(), 0.0);
        profile.gexPerStrike.resize(uniqueStrikes.size(), 0.0);
        profile.callGex.resize(uniqueStrikes.size(), 0.0);
        profile.putGex.resize(uniqueStrikes.size(), 0.0);

        double maxCallOI = 0, maxPutOI = 0;
        double maxGex = -1e18;
        double maxPainVal = 1e18;

        for (size_t si = 0; si < uniqueStrikes.size(); ++si)
            profile.strikes[si] = uniqueStrikes[si];

        for (auto c : contracts) {  // copy so we can fill computed fields
            double T = c.daysToExpiry / 365.0;
            if (T <= 0) T = 1.0 / 365.0;

            // Compute BSM Greeks for this contract
            BSMInput inp;
            inp.spot        = spot;
            inp.strike      = c.strike;
            inp.rate        = riskFree;
            inp.vol         = c.impliedVol > 0 ? c.impliedVol : 0.25;
            inp.timeToExpiry= T;
            inp.divYield    = divYield;
            inp.optionType  = c.type;

            auto res = BlackScholes::compute(inp);
            c.gamma = res.gamma;
            c.delta = res.delta;
            c.vega  = res.vega;

            // GEX = gamma × OI × multiplier × spot² / 100
            // Sign convention: dealers short calls (+GEX), long puts (+GEX for puts)
            double gex = c.gamma * c.openInterest * c.multiplier * spot * spot / 100.0;
            double dex = c.delta * c.openInterest * c.multiplier * spot;
            double vex = c.vega  * c.openInterest * c.multiplier;

            // Dealer sign: short calls = positive GEX, short puts = negative GEX
            if (c.type == OptionType::Call) {
                gex  =  gex;  // positive
                profile.totalCallGEX += gex;
            } else {
                gex  = -gex;  // puts create negative GEX
                profile.totalPutGEX  += gex;
            }

            profile.totalGEX += gex;
            profile.totalDEX += dex;
            profile.totalVEX += vex;
            c.gexContribution = gex;
            c.dexContribution = dex;
            c.vexContribution = vex;

            // Per-strike accumulation
            auto it = std::lower_bound(uniqueStrikes.begin(), uniqueStrikes.end(),
                                        c.strike);
            if (it != uniqueStrikes.end() && *it == c.strike) {
                size_t idx = static_cast<size_t>(it - uniqueStrikes.begin());
                profile.gexPerStrike[idx] += gex;
                if (c.type == OptionType::Call) profile.callGex[idx] += gex;
                else                            profile.putGex[idx]  += gex;
            }

            // Track key levels
            if (c.type == OptionType::Call && c.openInterest > maxCallOI) {
                maxCallOI = c.openInterest;
                profile.callWallStrike = c.strike;
            }
            if (c.type == OptionType::Put  && c.openInterest > maxPutOI) {
                maxPutOI = c.openInterest;
                profile.putWallStrike = c.strike;
            }
        }

        // Gamma wall = strike with largest positive net GEX
        for (size_t i = 0; i < profile.gexPerStrike.size(); ++i) {
            if (profile.gexPerStrike[i] > maxGex) {
                maxGex = profile.gexPerStrike[i];
                profile.gammaWallStrike = uniqueStrikes[i];
            }
        }

        // Zero-gamma level: interpolate where gexPerStrike changes sign
        profile.zeroGammaLevel = findZeroGamma(uniqueStrikes,
                                                profile.gexPerStrike, spot);

        // Max pain: strike minimising sum of in-the-money OI × intrinsic value
        profile.maxPainStrike = computeMaxPain(contracts, uniqueStrikes);

        // Scale to billions for GEX, millions for DEX/VEX
        profile.totalGEX      /= 1e9;
        profile.totalCallGEX  /= 1e9;
        profile.totalPutGEX   /= 1e9;
        profile.totalDEX      /= 1e6;
        profile.totalVEX      /= 1e6;

        return profile;
    }

private:
    static double findZeroGamma(const std::vector<double>& strikes,
                                  const std::vector<double>& gex,
                                  double spot) {
        if (strikes.empty()) return spot;

        // Find the strike range closest to spot where GEX changes sign
        for (size_t i = 1; i < strikes.size(); ++i) {
            if (gex[i-1] * gex[i] < 0) {
                // Linear interpolation
                double w = -gex[i-1] / (gex[i] - gex[i-1]);
                return strikes[i-1] + w * (strikes[i] - strikes[i-1]);
            }
        }
        return spot;  // No zero crossing found near spot
    }

    static double computeMaxPain(const std::vector<OptionContract>& contracts,
                                  const std::vector<double>& strikes) {
        if (strikes.empty()) return 0.0;

        double minPain = 1e18;
        double mpStrike = strikes[0];

        for (double testStrike : strikes) {
            double totalPain = 0.0;
            for (auto& c : contracts) {
                double intrinsic = 0.0;
                if (c.type == OptionType::Call)
                    intrinsic = std::max(0.0, testStrike - c.strike);
                else
                    intrinsic = std::max(0.0, c.strike - testStrike);
                totalPain += c.openInterest * intrinsic;
            }
            if (totalPain < minPain) {
                minPain   = totalPain;
                mpStrike  = testStrike;
            }
        }
        return mpStrike;
    }
};

} // namespace qtl
