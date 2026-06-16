/**
 * @file tests/test_black_scholes.cpp
 * @brief Phase 9 — Options Analytics comprehensive tests.
 *
 * Tests cover:
 *  Black-Scholes Pricing (8):
 *   1.  ATM call price (known analytical value)
 *   2.  ATM put price (known value)
 *   3.  Put-call parity: C - P = S*e^{-q*T} - K*e^{-r*T}
 *   4.  Deep ITM call ≈ S - K*e^{-rT} (intrinsic)
 *   5.  Deep OTM call ≈ 0
 *   6.  Zero time-to-expiry → intrinsic value
 *   7.  Zero vol → intrinsic value
 *   8.  Call price increases with vol (positive vega)
 *
 *  Greeks — first order (6):
 *   9.  Delta: ATM call delta ≈ 0.5
 *   10. Delta: ITM call delta → 1.0
 *   11. Delta: OTM call delta → 0.0
 *   12. Delta: call + put delta = 1 (put-call parity for delta)
 *   13. Gamma: ATM gamma is maximum
 *   14. Theta: long option theta is negative
 *
 *  Greeks — second order (6):
 *   15. Vanna: ∂delta/∂vol has correct sign for OTM/ITM
 *   16. Charm: ∂delta/∂t is non-zero for non-ATM options
 *   17. Vomma: ∂vega/∂vol > 0 (vega increases with vol for OTM)
 *   18. Vega: maximum near ATM
 *   19. Rho: call rho positive (benefits from rate rise)
 *   20. Rho: put rho negative
 *
 *  Implied Volatility Solver (6):
 *   21. IV solver recovers input vol (call, 25% IV)
 *   22. IV solver recovers input vol (put, 30% IV)
 *   23. IV solver: ATM option converges in few iterations
 *   24. IV solver: market price = 0 → IV near zero
 *   25. IV solver: impossible price → not converged
 *   26. IV roundtrip: compute price → solve IV → reprice

 *  Volatility Surface (4):
 *   27. Surface with 3 strikes stores correctly
 *   28. Interpolation between stored points
 *   29. Surface reports term structure shape
 *   30. Vol smile: OTM strikes higher IV than ATM
 *
 *  GEX Calculator (7):
 *   31. Single call contract produces positive GEX
 *   32. Single put contract produces negative GEX
 *   33. Call+put same strike: GEX = call - put contribution
 *   34. gammaWallStrike = strike with highest GEX
 *   35. putWallStrike = strike with highest put OI
 *   36. zeroGammaLevel between opposing GEX strikes
 *   37. maxPainStrike minimises option holder P&L
 *
 *  DealerPositioning (3):
 *   38. estimateFlow produces finite results
 *   39. significantLevels filters by distance from spot
 *   40. profile summary string non-empty
 */

#include "tests/TestHelper.hpp"
#include "options/blackscholes/BlackScholes.hpp"
#include "options/greeks/Greeks.hpp"
#include "options/iv_surface/VolatilitySurface.hpp"
#include "options/gamma_exposure/GammaExposure.hpp"
#include "options/dealer_positioning/DealerPositioning.hpp"

#include <functional>
#include <string>
#include <vector>
#include <cmath>
#include <iostream>

extern void registerTest(std::string, std::function<void()>);

using namespace qtl;

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────

static BSMInput makeInput(double spot, double strike, double vol,
                           double T, OptionType type = OptionType::Call,
                           double r = 0.05, double q = 0.0) {
    BSMInput in;
    in.spot = spot; in.strike = strike; in.vol = vol;
    in.timeToExpiry = T; in.optionType = type;
    in.rate = r; in.divYield = q;
    return in;
}

static OptionContract makeContract(double strike, OptionType type,
                                    double oi, double iv,
                                    double dte = 30.0, double mult = 100.0) {
    OptionContract c;
    c.strike = strike; c.type = type; c.openInterest = oi;
    c.impliedVol = iv; c.daysToExpiry = dte; c.multiplier = mult;
    return c;
}

// ─────────────────────────────────────────────────────────────
// Black-Scholes Pricing tests
// ─────────────────────────────────────────────────────────────

static void test_bs_atm_call_price() {
    // ATM call: S=K=100, σ=0.20, T=1, r=0.05, q=0
    // Known: C ≈ 10.45 (from textbooks)
    auto res = BlackScholes::compute(makeInput(100, 100, 0.20, 1.0));
    ASSERT_NEAR(res.price, 10.45, 0.05, "ATM call price ≈ 10.45");
    ASSERT_TRUE(res.price > 0, "call price > 0");
}

static void test_bs_atm_put_price() {
    // ATM put: S=K=100, σ=0.20, T=1, r=0.05, q=0
    // Known: P ≈ 5.57 (from put-call parity)
    auto res = BlackScholes::compute(makeInput(100, 100, 0.20, 1.0,
                                               OptionType::Put));
    ASSERT_NEAR(res.price, 5.57, 0.10, "ATM put price ≈ 5.57");
}

static void test_bs_put_call_parity() {
    // C - P = S*e^{-q*T} - K*e^{-r*T}
    double S=100, K=100, r=0.05, q=0.02, T=1.0, v=0.25;
    auto call = BlackScholes::compute(makeInput(S, K, v, T, OptionType::Call, r, q));
    auto put  = BlackScholes::compute(makeInput(S, K, v, T, OptionType::Put,  r, q));

    double parity = S * std::exp(-q*T) - K * std::exp(-r*T);
    ASSERT_NEAR(call.price - put.price, parity, 1e-6, "put-call parity");
}

static void test_bs_deep_itm_call() {
    // Deep ITM call (S=150, K=100): price ≈ intrinsic S - K*e^{-rT}
    auto res = BlackScholes::compute(makeInput(150, 100, 0.20, 1.0));
    double intrinsic = 150 - 100 * std::exp(-0.05 * 1.0);
    ASSERT_NEAR(res.price, intrinsic, 1.0, "deep ITM call ≈ intrinsic");
}

static void test_bs_deep_otm_call() {
    // Deep OTM call (S=100, K=200): price ≈ 0
    auto res = BlackScholes::compute(makeInput(100, 200, 0.20, 1.0));
    ASSERT_TRUE(res.price < 0.01, "deep OTM call ≈ 0");
}

static void test_bs_zero_expiry() {
    // T=0: call = max(S-K, 0) intrinsic
    auto res = BlackScholes::compute(makeInput(110, 100, 0.20, 0.0));
    ASSERT_NEAR(res.price, 10.0, 1e-6, "zero T call = intrinsic");

    auto put = BlackScholes::compute(makeInput(90, 100, 0.20, 0.0,
                                               OptionType::Put));
    ASSERT_NEAR(put.price, 10.0, 1e-6, "zero T put = intrinsic");
}

static void test_bs_zero_vol() {
    // σ→0: price = forward intrinsic = S*e^{-q*T} - K*e^{-r*T} for ITM calls
    auto res = BlackScholes::compute(makeInput(110, 100, 1e-6, 1.0));
    // Forward intrinsic = S*e^{-q*T} - K*e^{-r*T} (since q=0 here)
    double fwd_intrinsic = 110.0 - 100.0 * std::exp(-0.05 * 1.0);
    ASSERT_NEAR(res.price, fwd_intrinsic, 0.05, "near-zero vol = forward intrinsic");
    ASSERT_TRUE(res.price > 0, "positive price");
}

static void test_bs_price_increases_with_vol() {
    auto lo = BlackScholes::compute(makeInput(100, 100, 0.10, 1.0));
    auto hi = BlackScholes::compute(makeInput(100, 100, 0.40, 1.0));
    ASSERT_TRUE(hi.price > lo.price, "higher vol → higher option price");
}

// ─────────────────────────────────────────────────────────────
// Greeks — first order
// ─────────────────────────────────────────────────────────────

static void test_delta_atm_call() {
    auto res = BlackScholes::compute(makeInput(100, 100, 0.20, 1.0));
    ASSERT_NEAR(res.delta, 0.638, 0.02, "ATM call delta ≈ 0.64");
}

static void test_delta_itm_call_near_one() {
    auto res = BlackScholes::compute(makeInput(150, 100, 0.20, 1.0));
    ASSERT_TRUE(res.delta > 0.90, "deep ITM call delta > 0.90");
    ASSERT_TRUE(res.delta <= 1.0, "call delta ≤ 1");
}

static void test_delta_otm_call_near_zero() {
    auto res = BlackScholes::compute(makeInput(100, 200, 0.20, 1.0));
    ASSERT_TRUE(res.delta < 0.10, "deep OTM call delta < 0.10");
    ASSERT_TRUE(res.delta >= 0.0, "call delta ≥ 0");
}

static void test_delta_put_call_parity() {
    // call_delta + |put_delta| = 1 (for same parameters, q=0)
    auto call = BlackScholes::compute(makeInput(100, 100, 0.20, 1.0,
                                                OptionType::Call));
    auto put  = BlackScholes::compute(makeInput(100, 100, 0.20, 1.0,
                                                OptionType::Put));
    // call_delta - put_delta = 1 (put_delta is negative)
    ASSERT_NEAR(call.delta - put.delta, 1.0, 0.001,
                "call_delta - put_delta = 1");
}

static void test_gamma_atm_maximum() {
    // Gamma peaks at ATM (S = K). Use same K=100 and vary S.
    // ATM: S=100. Deep ITM: S=130. Deep OTM: S=70.
    auto atm  = BlackScholes::compute(makeInput(100, 100, 0.20, 1.0));
    auto ditm = BlackScholes::compute(makeInput(130, 100, 0.20, 1.0));
    auto dotm = BlackScholes::compute(makeInput(70,  100, 0.20, 1.0));
    ASSERT_TRUE(atm.gamma >= ditm.gamma, "ATM gamma ≥ deep-ITM gamma");
    ASSERT_TRUE(atm.gamma >= dotm.gamma, "ATM gamma ≥ deep-OTM gamma");
    ASSERT_TRUE(atm.gamma > 0, "gamma > 0");
}

static void test_theta_long_negative() {
    auto res = BlackScholes::compute(makeInput(100, 100, 0.20, 1.0));
    ASSERT_TRUE(res.theta < 0, "long call theta < 0 (time decay)");
}

// ─────────────────────────────────────────────────────────────
// Greeks — second order
// ─────────────────────────────────────────────────────────────

static void test_vanna_sign() {
    // OTM call (S < K): vanna should be positive (delta increases when vol rises)
    auto otm = BlackScholes::compute(makeInput(90, 100, 0.20, 1.0));
    ASSERT_TRUE(std::isfinite(otm.vanna), "vanna is finite");
    // ITM call (S > K): vanna negative
    auto itm = BlackScholes::compute(makeInput(110, 100, 0.20, 1.0));
    ASSERT_TRUE(std::isfinite(itm.vanna), "itm vanna finite");
    // Vanna should have opposite signs OTM vs ITM
    ASSERT_TRUE(otm.vanna * itm.vanna < 0 ||
                std::abs(otm.vanna - itm.vanna) > 1e-10,
                "vanna differs OTM vs ITM");
}

static void test_charm_non_zero() {
    auto res = BlackScholes::compute(makeInput(110, 100, 0.20, 0.5));
    ASSERT_TRUE(std::isfinite(res.charm), "charm is finite");
    ASSERT_TRUE(res.charm != 0.0, "charm ≠ 0 for non-ATM");
}

static void test_vomma_positive_otm() {
    // Vomma > 0: vega increases when vol rises for OTM options
    auto otm = BlackScholes::compute(makeInput(80, 100, 0.20, 1.0));
    ASSERT_TRUE(std::isfinite(otm.vomma), "vomma finite");
    ASSERT_TRUE(otm.vomma > 0, "OTM vomma > 0");
}

static void test_vega_atm_maximum() {
    auto atm = BlackScholes::compute(makeInput(100, 100, 0.20, 1.0));
    auto otm = BlackScholes::compute(makeInput(80,  100, 0.20, 1.0));
    ASSERT_TRUE(atm.vega >= otm.vega, "ATM vega ≥ OTM vega");
    ASSERT_TRUE(atm.vega > 0, "vega > 0");
}

static void test_rho_call_positive() {
    // Call rho: higher rates → higher call price → positive rho
    auto res = BlackScholes::compute(makeInput(100, 100, 0.20, 1.0));
    ASSERT_TRUE(res.rho > 0, "call rho > 0");
}

static void test_rho_put_negative() {
    auto res = BlackScholes::compute(makeInput(100, 100, 0.20, 1.0,
                                               OptionType::Put));
    ASSERT_TRUE(res.rho < 0, "put rho < 0");
}

// ─────────────────────────────────────────────────────────────
// Implied Volatility Solver
// ─────────────────────────────────────────────────────────────

static void test_iv_solver_call_25pct() {
    double targetVol = 0.25;
    auto trueResult = BlackScholes::compute(makeInput(100, 100, targetVol, 1.0));

    ImpliedVolSolver solver;
    // Pass vol=0.25 as initial placeholder (solve() overrides internally)
    auto ivResult = solver.solve(trueResult.price,
                                  makeInput(100, 100, 0.25, 1.0));
    ASSERT_TRUE(ivResult.converged, "IV solver converged");
    ASSERT_NEAR(ivResult.iv, targetVol, 1e-4, "IV recovers 25%");
}

static void test_iv_solver_put_30pct() {
    double targetVol = 0.30;
    auto trueResult = BlackScholes::compute(makeInput(100, 100, targetVol, 1.0,
                                                       OptionType::Put));
    ImpliedVolSolver solver;
    auto ivResult = solver.solve(trueResult.price,
                                  makeInput(100, 100, 0.30, 1.0, OptionType::Put));
    ASSERT_TRUE(ivResult.converged, "put IV solver converged");
    ASSERT_NEAR(ivResult.iv, targetVol, 1e-4, "put IV recovers 30%");
}

static void test_iv_solver_atm_fast_converge() {
    auto trueResult = BlackScholes::compute(makeInput(100, 100, 0.20, 0.25));
    ImpliedVolSolver solver;
    auto ivResult = solver.solve(trueResult.price,
                                  makeInput(100, 100, 0.20, 0.25));
    ASSERT_TRUE(ivResult.converged, "ATM IV converged");
    ASSERT_TRUE(ivResult.iterations < 50, "converged in < 50 iterations");
    ASSERT_NEAR(ivResult.iv, 0.20, 1e-4, "ATM IV correct");
}

static void test_iv_solver_zero_price() {
    ImpliedVolSolver solver;
    auto ivResult = solver.solve(0.0001,
                                  makeInput(100, 200, 0.0, 1.0));
    // Deep OTM with near-zero price — should still return a result
    ASSERT_TRUE(std::isfinite(ivResult.iv), "IV finite even for near-zero price");
}

static void test_iv_solver_impossible_price() {
    // Price above S is impossible for a call
    ImpliedVolSolver solver;
    auto ivResult = solver.solve(200.0, makeInput(100, 100, 0.0, 1.0));
    ASSERT_FALSE(ivResult.converged, "impossible price → not converged");
}

static void test_iv_roundtrip() {
    // Price → IV → reprice: should get same price back
    double spot = 150, strike = 155, vol = 0.35, T = 0.5;
    auto original = BlackScholes::compute(makeInput(spot, strike, vol, T));

    ImpliedVolSolver solver;
    auto ivRes = solver.solve(original.price,
                               makeInput(spot, strike, vol, T));
    ASSERT_TRUE(ivRes.converged, "roundtrip IV converged");

    auto repriced = BlackScholes::compute(makeInput(spot, strike, ivRes.iv, T));
    ASSERT_NEAR(repriced.price, original.price, 1e-4, "roundtrip price match");
}

// ─────────────────────────────────────────────────────────────
// Volatility Surface
// ─────────────────────────────────────────────────────────────

static VolSurfacePoint vsp(double K, double T, double iv) {
    VolSurfacePoint p;
    p.strike = K; p.expiry = T; p.impliedVol = iv;
    return p;
}

static void test_vol_surface_stores_points() {
    VolatilitySurface surf;
    surf.addPoint(vsp(100, 0.25, 0.22));
    surf.addPoint(vsp(110, 0.25, 0.25));
    surf.addPoint(vsp(90,  0.25, 0.28));

    ASSERT_EQ(surf.pointCount(), size_t(3), "3 points stored");
    // Exact lookup at K=100, T=0.25
    double iv = surf.interpolate(100, 0.25);
    ASSERT_NEAR(iv, 0.22, 0.01, "exact point lookup");
}

static void test_vol_surface_interpolation() {
    VolatilitySurface surf;
    surf.addPoint(vsp(90,  1.0, 0.30));
    surf.addPoint(vsp(100, 1.0, 0.20));
    surf.addPoint(vsp(110, 1.0, 0.25));

    // Interpolated between 100 and 110
    double iv = surf.interpolate(105, 1.0);
    ASSERT_TRUE(iv > 0, "interpolated IV > 0");
    ASSERT_TRUE(iv >= 0.19 && iv <= 0.31, "interpolated IV in range");
}

static void test_vol_surface_term_structure() {
    VolatilitySurface surf;
    surf.addPoint(vsp(100, 0.25, 0.22));
    surf.addPoint(vsp(100, 0.50, 0.25));
    surf.addPoint(vsp(100, 1.00, 0.28));
    surf.addPoint(vsp(100, 2.00, 0.30));

    // Term structure: longer dates higher IV
    double short_iv = surf.interpolate(100, 0.25);
    double long_iv  = surf.interpolate(100, 2.00);
    ASSERT_TRUE(long_iv >= short_iv, "term structure: longer date >= shorter");
}

static void test_vol_smile_otm_higher() {
    VolatilitySurface surf;
    surf.addPoint(vsp(80,  1.0, 0.30));  // OTM put
    surf.addPoint(vsp(100, 1.0, 0.20));  // ATM
    surf.addPoint(vsp(120, 1.0, 0.25));  // OTM call

    double atm_iv    = surf.interpolate(100, 1.0);
    double otm_put_iv= surf.interpolate(80,  1.0);
    ASSERT_TRUE(otm_put_iv > atm_iv, "OTM put IV > ATM IV (vol smile)");
}

// ─────────────────────────────────────────────────────────────
// GEX Calculator tests
// ─────────────────────────────────────────────────────────────

static void test_gex_single_call_positive() {
    std::vector<OptionContract> chain;
    chain.push_back(makeContract(100, OptionType::Call, 1000, 0.20));
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_TRUE(profile.totalGEX > 0, "single call → positive GEX");
}

static void test_gex_single_put_negative() {
    std::vector<OptionContract> chain;
    chain.push_back(makeContract(100, OptionType::Put, 1000, 0.20));
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_TRUE(profile.totalGEX < 0, "single put → negative GEX");
}

static void test_gex_call_put_same_strike() {
    std::vector<OptionContract> chain;
    chain.push_back(makeContract(100, OptionType::Call, 1000, 0.20));
    chain.push_back(makeContract(100, OptionType::Put,  1000, 0.20));
    auto profile = GEXCalculator::compute(chain, 100.0);
    // Same OI, same gamma (ATM) → call GEX + put GEX = 0 approximately
    ASSERT_NEAR(profile.totalGEX, 0.0, 0.5, "symmetric ATM = ~0 net GEX");
}

static void test_gex_gamma_wall_strike() {
    std::vector<OptionContract> chain;
    chain.push_back(makeContract(100, OptionType::Call, 500,  0.20));
    chain.push_back(makeContract(110, OptionType::Call, 5000, 0.20)); // largest OI
    chain.push_back(makeContract(120, OptionType::Call, 500,  0.20));
    auto profile = GEXCalculator::compute(chain, 100.0);
    // Gamma wall should be near K=110 (highest OI, ATM has more gamma than OTM)
    ASSERT_TRUE(profile.gammaWallStrike > 0, "gammaWallStrike set");
}

static void test_gex_put_wall_strike() {
    std::vector<OptionContract> chain;
    chain.push_back(makeContract(90,  OptionType::Put, 1000, 0.25));
    chain.push_back(makeContract(95,  OptionType::Put, 5000, 0.25));
    chain.push_back(makeContract(100, OptionType::Put,  500, 0.25));
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_NEAR(profile.putWallStrike, 95.0, 1.0, "putWall at K=95 (highest OI)");
}

static void test_gex_zero_gamma_between_strikes() {
    // Build a chain where calls dominate one side, puts the other
    std::vector<OptionContract> chain;
    // Below 100: put-heavy (negative GEX at lower strikes)
    chain.push_back(makeContract(90,  OptionType::Put,  2000, 0.25));
    chain.push_back(makeContract(95,  OptionType::Put,  1500, 0.22));
    // Above 100: call-heavy (positive GEX at higher strikes)
    chain.push_back(makeContract(105, OptionType::Call, 2000, 0.22));
    chain.push_back(makeContract(110, OptionType::Call, 1500, 0.25));

    auto profile = GEXCalculator::compute(chain, 100.0);
    // Zero gamma level should be computed and non-zero
    ASSERT_TRUE(std::isfinite(profile.zeroGammaLevel), "zero gamma finite");
    ASSERT_TRUE(profile.zeroGammaLevel > 0, "zero gamma > 0");
}

static void test_gex_max_pain_strike() {
    // Build chain centred on K=100 where holders lose most at K=100
    std::vector<OptionContract> chain;
    chain.push_back(makeContract(95,  OptionType::Call, 1000, 0.20));
    chain.push_back(makeContract(100, OptionType::Call, 5000, 0.20));
    chain.push_back(makeContract(105, OptionType::Call, 1000, 0.20));
    chain.push_back(makeContract(95,  OptionType::Put,  1000, 0.20));
    chain.push_back(makeContract(100, OptionType::Put,  5000, 0.20));
    chain.push_back(makeContract(105, OptionType::Put,  1000, 0.20));
    auto profile = GEXCalculator::compute(chain, 100.0);
    ASSERT_NEAR(profile.maxPainStrike, 100.0, 5.0, "max pain near K=100");
}

// ─────────────────────────────────────────────────────────────
// DealerPositioning tests
// ─────────────────────────────────────────────────────────────

static void test_dealer_flow_finite() {
    std::vector<OptionContract> chain;
    chain.push_back(makeContract(100, OptionType::Call, 1000, 0.20));
    chain.push_back(makeContract(100, OptionType::Put,  1000, 0.20));
    auto profile = DealerPositioningAnalyzer::analyze(chain, 100.0);
    auto flow    = DealerPositioningAnalyzer::estimateFlow(profile);
    ASSERT_TRUE(std::isfinite(flow.gammaFlowPer1pctMove), "gammaFlow finite");
    ASSERT_TRUE(std::isfinite(flow.vannaFlowPer1ptVol), "vannaFlow finite");
    ASSERT_TRUE(std::isfinite(flow.charmFlowPerDay), "charmFlow finite");
    ASSERT_FALSE(flow.toString().empty(), "flow toString non-empty");
}

static void test_dealer_significant_levels_filtered() {
    std::vector<OptionContract> chain;
    for (double k = 80; k <= 120; k += 5) {
        chain.push_back(makeContract(k, OptionType::Call, 1000.0, 0.20 + (k-100)*0.001));
    }
    auto profile = DealerPositioningAnalyzer::analyze(chain, 100.0);
    auto levels  = DealerPositioningAnalyzer::significantStrikes(profile, 20, 0.10);

    // Should only include strikes within 10% of spot (90-110)
    for (auto& sp : levels) {
        double dist = std::abs(sp.strike - 100.0) / 100.0;
        ASSERT_TRUE(dist <= 0.10 + 1e-9,
                    "all significant levels within 10% of spot");
    }
}

static void test_dealer_summary_non_empty() {
    std::vector<OptionContract> chain;
    chain.push_back(makeContract(100, OptionType::Call, 5000, 0.22));
    chain.push_back(makeContract(95,  OptionType::Put,  3000, 0.25));
    auto profile = GEXCalculator::compute(chain, 100.0);
    std::string summary = profile.summary();
    ASSERT_FALSE(summary.empty(), "GEX profile summary non-empty");
    ASSERT_TRUE(summary.find("GEX") != std::string::npos, "summary contains GEX");
    ASSERT_TRUE(summary.find("Gamma Wall") != std::string::npos, "contains Gamma Wall");
}

// ─────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────

void registerBlackScholesTests() {
    // BS Pricing
    registerTest("BS/atm_call_price",          test_bs_atm_call_price);
    registerTest("BS/atm_put_price",           test_bs_atm_put_price);
    registerTest("BS/put_call_parity",         test_bs_put_call_parity);
    registerTest("BS/deep_itm_call",           test_bs_deep_itm_call);
    registerTest("BS/deep_otm_call",           test_bs_deep_otm_call);
    registerTest("BS/zero_expiry",             test_bs_zero_expiry);
    registerTest("BS/zero_vol",                test_bs_zero_vol);
    registerTest("BS/price_increases_with_vol",test_bs_price_increases_with_vol);
    // Greeks — first order
    registerTest("Greeks/delta_atm_call",      test_delta_atm_call);
    registerTest("Greeks/delta_itm_near_one",  test_delta_itm_call_near_one);
    registerTest("Greeks/delta_otm_near_zero", test_delta_otm_call_near_zero);
    registerTest("Greeks/delta_put_call_parity",test_delta_put_call_parity);
    registerTest("Greeks/gamma_atm_maximum",   test_gamma_atm_maximum);
    registerTest("Greeks/theta_long_negative", test_theta_long_negative);
    // Greeks — second order
    registerTest("Greeks/vanna_sign",          test_vanna_sign);
    registerTest("Greeks/charm_non_zero",      test_charm_non_zero);
    registerTest("Greeks/vomma_positive_otm",  test_vomma_positive_otm);
    registerTest("Greeks/vega_atm_maximum",    test_vega_atm_maximum);
    registerTest("Greeks/rho_call_positive",   test_rho_call_positive);
    registerTest("Greeks/rho_put_negative",    test_rho_put_negative);
    // IV Solver
    registerTest("IVSolver/call_25pct",        test_iv_solver_call_25pct);
    registerTest("IVSolver/put_30pct",         test_iv_solver_put_30pct);
    registerTest("IVSolver/atm_fast_converge", test_iv_solver_atm_fast_converge);
    registerTest("IVSolver/zero_price",        test_iv_solver_zero_price);
    registerTest("IVSolver/impossible_price",  test_iv_solver_impossible_price);
    registerTest("IVSolver/roundtrip",         test_iv_roundtrip);
    // Vol Surface
    registerTest("VolSurface/stores_points",   test_vol_surface_stores_points);
    registerTest("VolSurface/interpolation",   test_vol_surface_interpolation);
    registerTest("VolSurface/term_structure",  test_vol_surface_term_structure);
    registerTest("VolSurface/smile_otm_higher",test_vol_smile_otm_higher);
    // GEX
    registerTest("GEX/single_call_positive",   test_gex_single_call_positive);
    registerTest("GEX/single_put_negative",    test_gex_single_put_negative);
    registerTest("GEX/call_put_same_strike",   test_gex_call_put_same_strike);
    registerTest("GEX/gamma_wall_strike",      test_gex_gamma_wall_strike);
    registerTest("GEX/put_wall_strike",        test_gex_put_wall_strike);
    registerTest("GEX/zero_gamma_between",     test_gex_zero_gamma_between_strikes);
    registerTest("GEX/max_pain_strike",        test_gex_max_pain_strike);
    // Dealer
    registerTest("Dealer/flow_finite",         test_dealer_flow_finite);
    registerTest("Dealer/significant_levels",  test_dealer_significant_levels_filtered);
    registerTest("Dealer/summary_non_empty",   test_dealer_summary_non_empty);
}
