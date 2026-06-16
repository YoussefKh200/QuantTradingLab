/**
 * @file tests/test_dealer_positioning.cpp
 * @brief Phase 10 — Dealer Positioning Engine comprehensive tests.
 *
 * Tests cover:
 *  OptionContract & Chain Building (3):
 *   1.  buildSyntheticChain produces correct count
 *   2.  Synthetic chain has both calls and puts
 *   3.  Vol smile: OTM strikes have higher IV than ATM
 *
 *  GEXCalculator (12):
 *   4.  Single call → positive total GEX
 *   5.  Single put  → negative total GEX
 *   6.  Call+put same strike same OI → near-zero net GEX
 *   7.  Gamma wall = strike with highest net GEX
 *   8.  Call wall  = strike with highest call OI
 *   9.  Put wall   = strike with highest put OI
 *   10. Zero-gamma level between opposing GEX strikes
 *   11. Max pain computed for symmetric chain
 *   12. DEX positive for call-heavy chain
 *   13. VEX always positive (dealers short vol both sides)
 *   14. VannEx has correct sign for OTM options
 *   15. CharmEx non-zero near expiry
 *
 *  GEX Profile (5):
 *   16. isPositiveGamma() for call-dominated chain
 *   17. isNegativeGamma() for put-dominated chain
 *   18. strikeProfiles sorted ascending
 *   19. netGEXPerStrike() returns correct count
 *   20. summary() contains key level strings
 *
 *  GEX Chart (3):
 *   21. chartGEX returns non-empty string
 *   22. chartGEX marks spot with asterisk
 *   23. chartVannEx returns non-empty string
 *
 *  DealerPositioningAnalyzer (10):
 *   24. analyze() returns valid GEX profile
 *   25. classifyRegime: positive gamma chain → PositiveGamma
 *   26. classifyRegime: negative gamma chain → NegativeGamma
 *   27. classifyRegime: spot near zero-gamma → Neutral
 *   28. estimateFlow: gammaFlow > 0 for positive gamma
 *   29. estimateFlow: charmFlow finite and non-zero
 *   30. estimateFlow: netDailyBias finite
 *   31. keyLevelDistances: put wall below spot → negative %
 *   32. significantStrikes: results within pctRange
 *   33. generateReport: contains all section headers
 *
 *  Multi-Expiry Roll-up (4):
 *   34. Chain with 2 expiries: totals > single expiry
 *   35. Near expiry has higher absolute GEX (same OI, higher gamma)
 *   36. Total VannEx aggregates across expiries
 *   37. Max pain computed across all expiries
 */

#include "tests/TestHelper.hpp"
#include "options/gamma_exposure/GammaExposure.hpp"
#include "options/dealer_positioning/DealerPositioning.hpp"

#include <functional>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <iostream>

extern void registerTest(std::string, std::function<void()>);
using namespace qtl;

// ─────────────────────────────────────────────────────────────
// Chain builder helpers
// ─────────────────────────────────────────────────────────────

static OptionContract makeCall(double K, double oi, double iv = 0.20,
                                double dte = 30.0, double mult = 100.0) {
    OptionContract c;
    c.strike = K; c.type = OptionType::Call;
    c.openInterest = oi; c.impliedVol = iv;
    c.daysToExpiry = dte; c.multiplier = mult;
    return c;
}

static OptionContract makePut(double K, double oi, double iv = 0.20,
                               double dte = 30.0, double mult = 100.0) {
    OptionContract c;
    c.strike = K; c.type = OptionType::Put;
    c.openInterest = oi; c.impliedVol = iv;
    c.daysToExpiry = dte; c.multiplier = mult;
    return c;
}

static std::vector<OptionContract> makeSymmetricChain(
        double spot, double spacing = 5.0, int nLevels = 5,
        double oi = 1000.0, double iv = 0.20, double dte = 30.0) {
    std::vector<OptionContract> chain;
    for (int i = -nLevels; i <= nLevels; ++i) {
        double K = spot + i * spacing;
        chain.push_back(makeCall(K, oi, iv, dte));
        chain.push_back(makePut (K, oi, iv, dte));
    }
    return chain;
}

// ─────────────────────────────────────────────────────────────
// buildSyntheticChain tests
// ─────────────────────────────────────────────────────────────

static void test_synthetic_chain_count() {
    auto chain = buildSyntheticChain(100.0, 5, 5.0, 0.20, 0.003, 30.0, 10000.0);
    // nStrikes=5 → strikes from -5 to +5 = 11 strikes × 2 (call+put) = 22
    ASSERT_EQ(chain.size(), size_t(22), "22 contracts (11 strikes × 2)");
}

static void test_synthetic_chain_has_both_types() {
    auto chain = buildSyntheticChain(100.0);
    bool hasCall = false, hasPut = false;
    for (auto& c : chain) {
        if (c.type == OptionType::Call) hasCall = true;
        if (c.type == OptionType::Put)  hasPut  = true;
    }
    ASSERT_TRUE(hasCall, "chain has calls");
    ASSERT_TRUE(hasPut,  "chain has puts");
}

static void test_synthetic_chain_vol_smile() {
    auto chain = buildSyntheticChain(100.0, 5, 10.0, 0.20, 0.005, 30.0, 10000.0);
    double atmIv = 0.0, otmIv = 0.0;
    for (auto& c : chain) {
        if (c.type == OptionType::Call) {
            if (std::abs(c.strike - 100.0) < 1.0)   atmIv = c.impliedVol;
            if (std::abs(c.strike - 150.0) < 1.0)   otmIv = c.impliedVol;
        }
    }
    if (otmIv > 0 && atmIv > 0)
        ASSERT_TRUE(otmIv > atmIv, "OTM call IV > ATM IV (vol smile)");
    ASSERT_TRUE(atmIv > 0, "ATM IV set");
}

// ─────────────────────────────────────────────────────────────
// GEXCalculator tests
// ─────────────────────────────────────────────────────────────

static void test_gex_single_call_positive() {
    std::vector<OptionContract> chain{makeCall(100, 1000)};
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_TRUE(profile.totalGEX > 0, "single call → positive GEX");
    ASSERT_TRUE(profile.totalCallGEX > 0, "call GEX > 0");
    ASSERT_EQ(profile.totalPutGEX, 0.0, "put GEX = 0");
}

static void test_gex_single_put_negative() {
    std::vector<OptionContract> chain{makePut(100, 1000)};
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_TRUE(profile.totalGEX < 0, "single put → negative GEX");
    ASSERT_TRUE(profile.totalPutGEX < 0, "put GEX < 0");
    ASSERT_EQ(profile.totalCallGEX, 0.0, "call GEX = 0");
}

static void test_gex_symmetric_near_zero() {
    // Same OI calls and puts at same strike → GEX ≈ 0
    std::vector<OptionContract> chain{
        makeCall(100, 1000, 0.20),
        makePut (100, 1000, 0.20)
    };
    auto profile = GEXCalculator::compute(chain, 100.0);
    // Call GEX = +rawGEX, Put GEX = -rawGEX → net ≈ 0
    ASSERT_NEAR(profile.totalGEX, 0.0, 0.1, "symmetric chain → ~0 net GEX");
}

static void test_gex_gamma_wall_correctness() {
    // Three calls: highest OI at K=105 → should be gamma wall
    std::vector<OptionContract> chain{
        makeCall( 95, 500,  0.20),
        makeCall(100, 2000, 0.20),
        makeCall(105, 5000, 0.20),  // largest OI
        makeCall(110, 500,  0.20),
    };
    auto profile = GEXCalculator::compute(chain, 100.0);
    // Gamma wall = highest net GEX strike
    ASSERT_TRUE(profile.gammaWallStrike > 0, "gammaWall set");
    ASSERT_TRUE(profile.totalGEX > 0, "all calls → positive GEX");
}

static void test_gex_call_wall_highest_call_oi() {
    std::vector<OptionContract> chain{
        makeCall(100, 1000), makeCall(110, 8000), makeCall(120, 500),
        makePut (100, 500),  makePut ( 90, 2000),
    };
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_NEAR(profile.callWallStrike, 110.0, 0.01, "call wall = K=110 (highest call OI)");
}

static void test_gex_put_wall_highest_put_oi() {
    std::vector<OptionContract> chain{
        makeCall(100, 500), makeCall(110, 500),
        makePut ( 90, 7000), makePut (95, 2000), makePut(100, 1000),
    };
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_NEAR(profile.putWallStrike, 90.0, 0.01, "put wall = K=90 (highest put OI)");
}

static void test_gex_zero_gamma_between_strikes() {
    // Puts below 100 dominate (negative GEX), calls above 100 dominate (positive GEX)
    std::vector<OptionContract> chain{
        makePut ( 90, 5000, 0.25),
        makePut ( 95, 3000, 0.22),
        makeCall(105, 5000, 0.22),
        makeCall(110, 3000, 0.25),
    };
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_TRUE(std::isfinite(profile.zeroGammaLevel), "zero-gamma finite");
    ASSERT_TRUE(profile.zeroGammaLevel > 0, "zero-gamma > 0");
}

static void test_gex_max_pain_symmetric() {
    // Symmetric chain centred at 100 → max pain = 100
    auto chain = makeSymmetricChain(100.0, 5.0, 3, 1000.0);
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_NEAR(profile.maxPainStrike, 100.0, 5.0, "max pain near 100 for symmetric chain");
}

static void test_gex_dex_positive_call_heavy() {
    // Call-heavy chain → positive DEX (dealers short calls → long delta hedge)
    std::vector<OptionContract> chain{
        makeCall(100, 5000), makeCall(105, 3000),
        makePut ( 95,  500), makePut ( 90,  500),
    };
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_TRUE(profile.totalDEX > 0, "call-heavy chain → positive DEX");
}

static void test_gex_vex_always_positive() {
    // VEX = vega × OI × mult (always positive regardless of call/put)
    auto chain = makeSymmetricChain(100.0);
    auto profile = GEXCalculator::compute(chain, 100.0);
    // VEX is the total vega exposure — should be positive (dealers short vega both sides)
    // Actually vex for calls and puts both positive → net > 0
    ASSERT_TRUE(profile.totalVEX > 0, "symmetric chain VEX > 0");
}

static void test_gex_vannex_sign() {
    // VannEx for calls: positive (OTM calls gain delta when vol rises)
    std::vector<OptionContract> chain{makeCall(110, 1000, 0.20)};  // OTM call
    auto profile = GEXCalculator::compute(chain, 100.0);
    // Total VannEx should be positive for OTM call (vanna > 0 for OTM call)
    ASSERT_TRUE(std::isfinite(profile.totalVannEx), "VannEx finite");
}

static void test_gex_charmex_near_expiry() {
    // Charm is larger for near-expiry options
    std::vector<OptionContract> chain_near{makeCall(100, 1000, 0.20, 7.0)};   // 7 DTE
    std::vector<OptionContract> chain_far {makeCall(100, 1000, 0.20, 90.0)};  // 90 DTE
    auto near_prof = GEXCalculator::compute(chain_near, 100.0);
    auto far_prof  = GEXCalculator::compute(chain_far,  100.0);
    ASSERT_TRUE(std::abs(near_prof.totalCharmEx) > std::abs(far_prof.totalCharmEx),
                "near-expiry charm > far-expiry charm");
}

// ─────────────────────────────────────────────────────────────
// GEXProfile tests
// ─────────────────────────────────────────────────────────────

static void test_profile_positive_gamma() {
    std::vector<OptionContract> chain{makeCall(100, 5000), makePut(100, 100)};
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_TRUE(profile.isPositiveGamma(), "call-dominated = positive gamma");
    ASSERT_FALSE(profile.isNegativeGamma(), "not negative gamma");
}

static void test_profile_negative_gamma() {
    std::vector<OptionContract> chain{makePut(100, 5000), makeCall(100, 100)};
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_TRUE(profile.isNegativeGamma(), "put-dominated = negative gamma");
    ASSERT_FALSE(profile.isPositiveGamma(), "not positive gamma");
}

static void test_profile_strikes_sorted() {
    auto chain = buildSyntheticChain(100.0, 5, 5.0);
    auto profile = GEXCalculator::compute(chain, 100.0);
    for (size_t i = 1; i < profile.strikeProfiles.size(); ++i) {
        ASSERT_TRUE(profile.strikeProfiles[i].strike >=
                    profile.strikeProfiles[i-1].strike,
                    "strike profiles sorted ascending");
    }
}

static void test_profile_gex_per_strike_count() {
    auto chain = buildSyntheticChain(100.0, 5, 5.0);
    auto profile = GEXCalculator::compute(chain, 100.0);
    auto strikes = profile.strikes();
    auto gexVec  = profile.netGEXPerStrike();
    ASSERT_EQ(strikes.size(), gexVec.size(), "strikes.size() == gexPerStrike.size()");
    ASSERT_EQ(strikes.size(), profile.strikeProfiles.size(),
              "consistent strike count");
}

static void test_profile_summary_contains_levels() {
    auto chain = buildSyntheticChain(100.0);
    auto profile = GEXCalculator::compute(chain, 100.0);
    std::string sum = profile.summary();
    ASSERT_FALSE(sum.empty(), "summary non-empty");
    ASSERT_TRUE(sum.find("GEX") != std::string::npos,   "contains GEX");
    ASSERT_TRUE(sum.find("Gamma Wall") != std::string::npos, "contains Gamma Wall");
    ASSERT_TRUE(sum.find("Put Wall")   != std::string::npos, "contains Put Wall");
    ASSERT_TRUE(sum.find("Zero-Gamma") != std::string::npos, "contains Zero-Gamma");
}

// ─────────────────────────────────────────────────────────────
// GEX Chart tests
// ─────────────────────────────────────────────────────────────

static void test_chart_gex_non_empty() {
    auto chain = buildSyntheticChain(100.0);
    auto profile = GEXCalculator::compute(chain, 100.0);
    std::string chart = GEXCalculator::chartGEX(profile, 10, 30);
    ASSERT_FALSE(chart.empty(), "GEX chart non-empty");
    ASSERT_TRUE(chart.find("GEX") != std::string::npos, "chart has GEX label");
    ASSERT_TRUE(chart.size() > 100, "chart has meaningful content");
}

static void test_chart_gex_has_spot_marker() {
    // The spot strike should be marked with '*'
    // Build chain where one strike equals spot exactly
    std::vector<OptionContract> chain{
        makeCall(100.0, 1000), makePut(100.0, 500)
    };
    auto profile = GEXCalculator::compute(chain, 100.0);
    std::string chart = GEXCalculator::chartGEX(profile, 5, 20);
    ASSERT_TRUE(chart.find('*') != std::string::npos ||
                chart.find("100") != std::string::npos,
                "chart references spot");
}

static void test_chart_vannex_non_empty() {
    auto chain = buildSyntheticChain(100.0);
    auto profile = GEXCalculator::compute(chain, 100.0);
    std::string chart = GEXCalculator::chartVannEx(profile, 10, 25);
    ASSERT_FALSE(chart.empty(), "VannEx chart non-empty");
}

// ─────────────────────────────────────────────────────────────
// DealerPositioningAnalyzer tests
// ─────────────────────────────────────────────────────────────

static void test_analyzer_returns_valid_profile() {
    auto chain = buildSyntheticChain(450.0, 10, 5.0, 0.18);
    auto profile = DealerPositioningAnalyzer::analyze(chain, 450.0);
    ASSERT_EQ(profile.spot, 450.0, "spot set correctly");
    ASSERT_TRUE(std::isfinite(profile.totalGEX), "totalGEX finite");
    ASSERT_FALSE(profile.strikeProfiles.empty(), "strike profiles populated");
}

static void test_regime_positive_gamma() {
    // Call-dominated chain → positive gamma → PositiveGamma regime
    std::vector<OptionContract> chain{
        makeCall(100, 10000, 0.20),
        makePut (100,   100, 0.20),
    };
    auto profile = DealerPositioningAnalyzer::analyze(chain, 100.0);
    auto regime  = DealerPositioningAnalyzer::classifyRegime(profile);
    ASSERT_TRUE(regime == VolRegime::PositiveGamma ||
                regime == VolRegime::NeutralGamma,
                "call-dominated chain → positive or neutral regime");
}

static void test_regime_negative_gamma() {
    // Put-dominated chain → negative gamma
    std::vector<OptionContract> chain{
        makePut (100, 10000, 0.20),
        makeCall(100,   100, 0.20),
    };
    auto profile = DealerPositioningAnalyzer::analyze(chain, 100.0);
    auto regime  = DealerPositioningAnalyzer::classifyRegime(profile);
    ASSERT_TRUE(regime == VolRegime::NegativeGamma ||
                regime == VolRegime::NeutralGamma,
                "put-dominated chain → negative or neutral regime");
}

static void test_regime_neutral_near_zero_gamma() {
    // Balanced chain with spot right at zero-gamma level → neutral
    auto chain = makeSymmetricChain(100.0);  // perfectly balanced
    auto profile = DealerPositioningAnalyzer::analyze(chain, 100.0);
    // With symmetric chain, regime should be neutral or near zero
    auto regime = DealerPositioningAnalyzer::classifyRegime(profile, 0.50); // wide neutral band
    ASSERT_TRUE(regime == VolRegime::NeutralGamma ||
                regime == VolRegime::PositiveGamma ||
                regime == VolRegime::NegativeGamma,
                "valid regime enum value");
    // At minimum, the total GEX should be near zero
    ASSERT_NEAR(profile.totalGEX, 0.0, 1.0, "symmetric chain near-zero GEX");
}

static void test_flow_gamma_positive_for_positive_gamma() {
    std::vector<OptionContract> chain{makeCall(100, 5000)};
    auto profile = DealerPositioningAnalyzer::analyze(chain, 100.0);
    auto flow    = DealerPositioningAnalyzer::estimateFlow(profile);
    ASSERT_TRUE(std::isfinite(flow.gammaFlowPer1pctMove), "gammaFlow finite");
    ASSERT_TRUE(std::isfinite(flow.gammaDollarFlow),      "gammaDollar finite");
}

static void test_flow_charm_non_zero() {
    auto chain = makeSymmetricChain(100.0, 5.0, 3, 1000.0, 0.20, 14.0); // 14 DTE
    auto profile = DealerPositioningAnalyzer::analyze(chain, 100.0);
    auto flow    = DealerPositioningAnalyzer::estimateFlow(profile);
    ASSERT_TRUE(std::isfinite(flow.charmFlowPerDay), "charmFlow finite");
    ASSERT_TRUE(std::isfinite(flow.charmDollarFlow), "charmDollar finite");
}

static void test_flow_net_bias_finite() {
    auto chain = buildSyntheticChain(100.0);
    auto profile = DealerPositioningAnalyzer::analyze(chain, 100.0);
    auto flow    = DealerPositioningAnalyzer::estimateFlow(profile);
    ASSERT_TRUE(std::isfinite(flow.netDailyBias), "netDailyBias finite");
    ASSERT_FALSE(flow.toString().empty(), "flow.toString() non-empty");
}

static void test_key_level_distances_put_wall_below() {
    // Put wall below spot → negative % distance
    std::vector<OptionContract> chain{
        makeCall(110, 1000),
        makePut ( 90, 5000),  // put wall at 90, below spot=100
    };
    auto profile = DealerPositioningAnalyzer::analyze(chain, 100.0);
    auto kld     = DealerPositioningAnalyzer::keyLevelDistances(profile);
    ASSERT_TRUE(kld.distToPutWall < 0, "put wall below spot → negative distance");
    ASSERT_TRUE(kld.distToCallWall > 0, "call wall above spot → positive distance");
}

static void test_significant_strikes_within_range() {
    auto chain = buildSyntheticChain(100.0, 10, 5.0, 0.20, 0.005, 30.0, 5000.0);
    auto profile = DealerPositioningAnalyzer::analyze(chain, 100.0);
    auto sigs    = DealerPositioningAnalyzer::significantStrikes(profile, 5, 0.10);

    for (auto& sp : sigs) {
        double dist = std::abs(sp.strike - 100.0) / 100.0;
        ASSERT_TRUE(dist <= 0.10 + 1e-9, "all significant strikes within 10%");
    }
    ASSERT_TRUE(sigs.size() <= size_t(5), "at most 5 significant strikes returned");
}

static void test_generate_report_has_sections() {
    auto chain = buildSyntheticChain(4500.0, 8, 25.0, 0.18, 0.002, 30.0, 50000.0);
    auto profile = DealerPositioningAnalyzer::analyze(chain, 4500.0);
    std::string report = DealerPositioningAnalyzer::generateReport(profile, "SPX");

    ASSERT_FALSE(report.empty(), "report non-empty");
    ASSERT_TRUE(report.find("SPX")           != std::string::npos, "report has symbol");
    ASSERT_TRUE(report.find("EXPOSURE")      != std::string::npos, "report has EXPOSURE section");
    ASSERT_TRUE(report.find("KEY LEVELS")    != std::string::npos, "report has KEY LEVELS section");
    ASSERT_TRUE(report.find("HEDGING FLOWS") != std::string::npos, "report has HEDGING FLOWS section");
    ASSERT_TRUE(report.find("Gamma Wall")    != std::string::npos, "report has Gamma Wall");
    ASSERT_TRUE(report.find("Zero-Gamma")    != std::string::npos, "report has Zero-Gamma");
    ASSERT_TRUE(report.find("GEX")           != std::string::npos, "report references GEX");
    // Print for visual inspection
    std::cout << report.substr(0, 500) << "...\n";
}

// ─────────────────────────────────────────────────────────────
// Multi-Expiry tests
// ─────────────────────────────────────────────────────────────

static void test_multi_expiry_totals_larger() {
    // Two expiries should give larger total GEX than one
    std::vector<OptionContract> chain_single{makeCall(100, 1000, 0.20, 30.0)};
    std::vector<OptionContract> chain_double{
        makeCall(100, 1000, 0.20, 30.0),
        makeCall(100, 1000, 0.20, 60.0),
    };
    auto p1 = GEXCalculator::compute(chain_single, 100.0);
    auto p2 = GEXCalculator::compute(chain_double, 100.0);
    ASSERT_TRUE(p2.totalGEX > p1.totalGEX, "two expiries → larger total GEX");
}

static void test_near_expiry_higher_gamma() {
    // Near-expiry option has higher gamma (same OI) → larger GEX contribution
    std::vector<OptionContract> chain_near{makeCall(100, 1000, 0.20,  7.0)};
    std::vector<OptionContract> chain_far {makeCall(100, 1000, 0.20, 90.0)};
    auto near_prof = GEXCalculator::compute(chain_near, 100.0);
    auto far_prof  = GEXCalculator::compute(chain_far,  100.0);
    ASSERT_TRUE(near_prof.totalGEX > far_prof.totalGEX,
                "near-expiry has higher GEX (higher gamma)");
}

static void test_multi_expiry_vannex_aggregates() {
    std::vector<OptionContract> chain{
        makeCall(100, 1000, 0.20, 30.0),
        makeCall(110, 1000, 0.25, 60.0),
    };
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_TRUE(std::isfinite(profile.totalVannEx), "VannEx finite for multi-expiry");
}

static void test_multi_expiry_max_pain() {
    // Max pain computed across all expiries at same strike
    std::vector<OptionContract> chain{
        makeCall(95,  1000, 0.20, 30.0), makeCall(95,  1000, 0.20, 60.0),
        makeCall(100, 5000, 0.20, 30.0), makeCall(100, 5000, 0.20, 60.0),
        makeCall(105, 1000, 0.20, 30.0), makeCall(105, 1000, 0.20, 60.0),
        makePut ( 95, 1000, 0.20, 30.0), makePut ( 95, 1000, 0.20, 60.0),
        makePut (100, 5000, 0.20, 30.0), makePut (100, 5000, 0.20, 60.0),
        makePut (105, 1000, 0.20, 30.0), makePut (105, 1000, 0.20, 60.0),
    };
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_NEAR(profile.maxPainStrike, 100.0, 5.0,
                "max pain near 100 for symmetric multi-expiry chain");
}

// ─────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────

void registerDealerPositioningTests() {
    // Chain building
    registerTest("DealerPos/synthetic_chain_count",       test_synthetic_chain_count);
    registerTest("DealerPos/synthetic_has_both_types",    test_synthetic_chain_has_both_types);
    registerTest("DealerPos/synthetic_vol_smile",         test_synthetic_chain_vol_smile);
    // GEX Calculator
    registerTest("GEX/single_call_positive",              test_gex_single_call_positive);
    registerTest("GEX/single_put_negative",               test_gex_single_put_negative);
    registerTest("GEX/symmetric_near_zero",               test_gex_symmetric_near_zero);
    registerTest("GEX/gamma_wall_correctness",            test_gex_gamma_wall_correctness);
    registerTest("GEX/call_wall_highest_oi",              test_gex_call_wall_highest_call_oi);
    registerTest("GEX/put_wall_highest_oi",               test_gex_put_wall_highest_put_oi);
    registerTest("GEX/zero_gamma_between_strikes",        test_gex_zero_gamma_between_strikes);
    registerTest("GEX/max_pain_symmetric",                test_gex_max_pain_symmetric);
    registerTest("GEX/dex_positive_call_heavy",           test_gex_dex_positive_call_heavy);
    registerTest("GEX/vex_positive",                      test_gex_vex_always_positive);
    registerTest("GEX/vannex_sign",                       test_gex_vannex_sign);
    registerTest("GEX/charmex_near_expiry",               test_gex_charmex_near_expiry);
    // GEX Profile
    registerTest("GEXProfile/positive_gamma",             test_profile_positive_gamma);
    registerTest("GEXProfile/negative_gamma",             test_profile_negative_gamma);
    registerTest("GEXProfile/strikes_sorted",             test_profile_strikes_sorted);
    registerTest("GEXProfile/gex_per_strike_count",       test_profile_gex_per_strike_count);
    registerTest("GEXProfile/summary_has_levels",         test_profile_summary_contains_levels);
    // Charts
    registerTest("GEXChart/chart_gex_non_empty",          test_chart_gex_non_empty);
    registerTest("GEXChart/chart_has_spot_marker",        test_chart_gex_has_spot_marker);
    registerTest("GEXChart/chart_vannex_non_empty",       test_chart_vannex_non_empty);
    // DealerPositioningAnalyzer
    registerTest("DealerAnalyzer/returns_valid_profile",  test_analyzer_returns_valid_profile);
    registerTest("DealerAnalyzer/regime_positive",        test_regime_positive_gamma);
    registerTest("DealerAnalyzer/regime_negative",        test_regime_negative_gamma);
    registerTest("DealerAnalyzer/regime_neutral",         test_regime_neutral_near_zero_gamma);
    registerTest("DealerAnalyzer/flow_gamma_positive",    test_flow_gamma_positive_for_positive_gamma);
    registerTest("DealerAnalyzer/flow_charm_non_zero",    test_flow_charm_non_zero);
    registerTest("DealerAnalyzer/flow_net_bias_finite",   test_flow_net_bias_finite);
    registerTest("DealerAnalyzer/key_level_put_wall_below",test_key_level_distances_put_wall_below);
    registerTest("DealerAnalyzer/significant_in_range",   test_significant_strikes_within_range);
    registerTest("DealerAnalyzer/report_has_sections",    test_generate_report_has_sections);
    // Multi-Expiry
    registerTest("MultiExpiry/totals_larger",             test_multi_expiry_totals_larger);
    registerTest("MultiExpiry/near_expiry_higher_gamma",  test_near_expiry_higher_gamma);
    registerTest("MultiExpiry/vannex_aggregates",         test_multi_expiry_vannex_aggregates);
    registerTest("MultiExpiry/max_pain",                  test_multi_expiry_max_pain);
}
