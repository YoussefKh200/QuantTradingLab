#pragma once
/**
 * @file options/blackscholes/BlackScholes.hpp
 * @brief Black-Scholes-Merton option pricing model.
 *
 * Implements the closed-form BSM formulae for European options:
 *
 *   Call price = S*e^{-q*T}*N(d1) - K*e^{-r*T}*N(d2)
 *   Put  price = K*e^{-r*T}*N(-d2) - S*e^{-q*T}*N(-d1)
 *
 *   d1 = [ln(S/K) + (r - q + σ²/2)*T] / (σ*√T)
 *   d2 = d1 - σ*√T
 *
 * where:
 *   S = spot price
 *   K = strike price
 *   r = continuously-compounded risk-free rate
 *   q = continuous dividend yield
 *   σ = implied/historical volatility (annualised)
 *   T = time to expiry in years
 *   N = cumulative standard normal CDF
 *
 * Numerical implementation
 * ────────────────────────
 * N(x) uses the Hart (1968) rational approximation — accurate to
 * ~7 decimal places, branch-free, suitable for hot loops.
 *
 * All functions are pure (no state, no allocation) and safe to call
 * from any thread.
 *
 * Edge cases handled:
 *   T ≤ 0   → intrinsic value (max(S-K,0) for call, max(K-S,0) for put)
 *   σ ≤ 0   → intrinsic value
 *   S ≤ 0   → 0
 *   K ≤ 0   → 0
 */

#include "core/Types.hpp"
#include <cmath>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <optional>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// BSM Input parameters
// ─────────────────────────────────────────────────────────────

struct BSMInput {
    double spot{0.0};       ///< Current underlying price S
    double strike{0.0};     ///< Option strike price K
    double rate{0.0};       ///< Risk-free rate r (annual, continuously compounded)
    double vol{0.0};        ///< Implied / historical volatility σ (annual)
    double timeToExpiry{0.0}; ///< Time to expiration T (in years)
    double divYield{0.0};   ///< Continuous dividend yield q
    OptionType optionType{OptionType::Call};

    [[nodiscard]] bool isValid() const noexcept {
        return spot > 0 && strike > 0 && vol > 0 && timeToExpiry > 0;
    }
};

// ─────────────────────────────────────────────────────────────
// BSM Output — price + first/second-order Greeks
// ─────────────────────────────────────────────────────────────

struct BSMResult {
    double price{0.0};

    // First-order Greeks
    double delta{0.0};   ///< ∂V/∂S
    double gamma{0.0};   ///< ∂²V/∂S²
    double theta{0.0};   ///< ∂V/∂t  (per calendar day, negative for long)
    double vega{0.0};    ///< ∂V/∂σ  (per 1% move in vol)
    double rho{0.0};     ///< ∂V/∂r  (per 1% move in rate)

    // Second-order / cross Greeks
    double vanna{0.0};   ///< ∂²V/(∂S∂σ) = ∂delta/∂σ = ∂vega/∂S
    double charm{0.0};   ///< ∂²V/(∂S∂t) = ∂delta/∂t (delta bleed)
    double vomma{0.0};   ///< ∂²V/∂σ²   = ∂vega/∂σ  (volga)
    double veta{0.0};    ///< ∂²V/(∂σ∂t) = ∂vega/∂t
    double speed{0.0};   ///< ∂³V/∂S³   = ∂gamma/∂S
    double zomma{0.0};   ///< ∂³V/(∂S²∂σ) = ∂gamma/∂σ
    double color{0.0};   ///< ∂³V/(∂S²∂t) = ∂gamma/∂t

    // Diagnostics
    double d1{0.0};
    double d2{0.0};
    double nd1{0.0};     ///< N(d1)
    double nd2{0.0};     ///< N(d2)
    double npd1{0.0};    ///< n(d1)  — standard normal PDF at d1
};

// ─────────────────────────────────────────────────────────────
// BlackScholes — all-static computation engine
// ─────────────────────────────────────────────────────────────

class BlackScholes {
public:

    // ── Normal distribution helpers ───────────────────────────

    /**
     * @brief Standard normal CDF using Hart's rational approximation.
     *        Max error ≈ 7.5e-8 for all x.
     */
    [[nodiscard]] static double normCdf(double x) noexcept {
        // Abramowitz & Stegun 26.2.17, Horner form
        constexpr double a1 =  0.319381530;
        constexpr double a2 = -0.356563782;
        constexpr double a3 =  1.781477937;
        constexpr double a4 = -1.821255978;
        constexpr double a5 =  1.330274429;
        constexpr double p  =  0.231641900;
        constexpr double c  =  0.398942280; // 1/sqrt(2π)

        double t = 1.0 / (1.0 + p * std::abs(x));
        double poly = t * (a1 + t * (a2 + t * (a3 + t * (a4 + t * a5))));
        double n = 1.0 - c * std::exp(-0.5 * x * x) * poly;
        return x >= 0.0 ? n : 1.0 - n;
    }

    /**
     * @brief Standard normal PDF.
     */
    [[nodiscard]] static double normPdf(double x) noexcept {
        constexpr double inv_sqrt2pi = 0.3989422804014327;
        return inv_sqrt2pi * std::exp(-0.5 * x * x);
    }

    // ── d1 / d2 ──────────────────────────────────────────────

    [[nodiscard]] static double calcD1(const BSMInput& in) noexcept {
        double sqrtT = std::sqrt(in.timeToExpiry);
        return (std::log(in.spot / in.strike) +
                (in.rate - in.divYield + 0.5 * in.vol * in.vol) *
                in.timeToExpiry) /
               (in.vol * sqrtT);
    }

    [[nodiscard]] static double calcD2(double d1, const BSMInput& in) noexcept {
        return d1 - in.vol * std::sqrt(in.timeToExpiry);
    }

    // ── Price ─────────────────────────────────────────────────

    /**
     * @brief Compute BSM price.
     * @param in  BSMInput (all fields required).
     * @return Option price in same currency as spot.
     */
    [[nodiscard]] static double price(const BSMInput& in) noexcept {
        if (!in.isValid()) {
            // Intrinsic value
            if (in.optionType == OptionType::Call)
                return std::max(in.spot - in.strike, 0.0);
            else
                return std::max(in.strike - in.spot, 0.0);
        }

        double sqrtT = std::sqrt(in.timeToExpiry);
        double d1    = calcD1(in);
        double d2    = d1 - in.vol * sqrtT;

        double discR = std::exp(-in.rate     * in.timeToExpiry);
        double discQ = std::exp(-in.divYield * in.timeToExpiry);

        if (in.optionType == OptionType::Call) {
            return in.spot * discQ * normCdf(d1) -
                   in.strike * discR * normCdf(d2);
        } else {
            return in.strike * discR * normCdf(-d2) -
                   in.spot * discQ * normCdf(-d1);
        }
    }

    // ── Full Greeks computation ───────────────────────────────

    /**
     * @brief Compute price and all Greeks in one pass.
     *
     * Computing all Greeks in one call amortises the cost of
     * computing shared intermediates (d1, d2, N(d1), n(d1), etc.)
     * across all 13 output fields.
     */
    [[nodiscard]] static BSMResult compute(const BSMInput& in) noexcept {
        BSMResult r;

        if (!in.isValid()) {
            if (in.optionType == OptionType::Call)
                r.price = std::max(in.spot - in.strike, 0.0);
            else
                r.price = std::max(in.strike - in.spot, 0.0);
            r.delta = (in.optionType == OptionType::Call &&
                       in.spot > in.strike) ? 1.0 : 0.0;
            return r;
        }

        double S     = in.spot;
        double K     = in.strike;
        double r_    = in.rate;
        double q     = in.divYield;
        double sig   = in.vol;
        double T     = in.timeToExpiry;

        double sqrtT = std::sqrt(T);
        double sigSqrtT = sig * sqrtT;

        double d1 = (std::log(S / K) + (r_ - q + 0.5 * sig * sig) * T) / sigSqrtT;
        double d2 = d1 - sigSqrtT;

        double Nd1  = normCdf(d1);
        double Nd2  = normCdf(d2);
        double Nnd1 = normCdf(-d1);
        double Nnd2 = normCdf(-d2);
        double nd1  = normPdf(d1);   // n(d1)

        double discR = std::exp(-r_ * T);
        double discQ = std::exp(-q  * T);
        double SdiscQ = S * discQ;
        double KdiscR = K * discR;

        r.d1   = d1;
        r.d2   = d2;
        r.nd1  = Nd1;
        r.nd2  = Nd2;
        r.npd1 = nd1;

        bool isCall = (in.optionType == OptionType::Call);

        // ── Price ─────────────────────────────────────────────
        r.price = isCall
            ? SdiscQ * Nd1 - KdiscR * Nd2
            : KdiscR * Nnd2 - SdiscQ * Nnd1;

        // ── Delta = ∂V/∂S ─────────────────────────────────────
        r.delta = isCall
            ? discQ * Nd1
            : discQ * (Nd1 - 1.0);   // = -discQ * N(-d1)

        // ── Gamma = ∂²V/∂S² (same for call and put) ──────────
        r.gamma = discQ * nd1 / (S * sigSqrtT);

        // ── Theta = ∂V/∂t  (per day, so divide by 365) ───────
        // Raw theta (per year):
        double thetaRaw_call =
            -(SdiscQ * nd1 * sig / (2.0 * sqrtT))
            - r_ * KdiscR * Nd2
            + q  * SdiscQ * Nd1;
        double thetaRaw_put =
            -(SdiscQ * nd1 * sig / (2.0 * sqrtT))
            + r_ * KdiscR * Nnd2
            - q  * SdiscQ * Nnd1;
        r.theta = (isCall ? thetaRaw_call : thetaRaw_put) / 365.0;

        // ── Vega = ∂V/∂σ  (per 1% move in vol) ──────────────
        // Raw vega (per unit of vol):
        double vegaRaw = SdiscQ * sqrtT * nd1;
        r.vega = vegaRaw / 100.0;   // per 1% vol move

        // ── Rho = ∂V/∂r  (per 1% move in rate) ──────────────
        r.rho = (isCall
            ?  KdiscR * T * Nd2
            : -KdiscR * T * Nnd2) / 100.0;

        // ── Vanna = ∂²V/(∂S∂σ) = vega / S * (1 - d1/sigSqrtT) ──
        r.vanna = -discQ * nd1 * d2 / sig;

        // ── Charm = ∂delta/∂t (per day) ──────────────────────
        double charmRaw = isCall
            ?  discQ * nd1 * (2.0*(r_-q)*T - d2*sigSqrtT) / (2.0*T*sigSqrtT)
               - q * discQ * Nd1
            : -discQ * nd1 * (2.0*(r_-q)*T - d2*sigSqrtT) / (2.0*T*sigSqrtT)
               + q * discQ * Nnd1;
        r.charm = charmRaw / 365.0;

        // ── Vomma (Volga) = ∂²V/∂σ² ─────────────────────────
        r.vomma = vegaRaw * d1 * d2 / sig / 100.0;

        // ── Veta = ∂vega/∂t (per day) ────────────────────────
        r.veta = -vegaRaw *
                  (q + (r_-q)*d1/sigSqrtT - (1.0 + d1*d2)/(2.0*T))
                  / 365.0 / 100.0;

        // ── Speed = ∂gamma/∂S ────────────────────────────────
        r.speed = -r.gamma / S * (1.0 + d1 / sigSqrtT);

        // ── Zomma = ∂gamma/∂σ ────────────────────────────────
        r.zomma = r.gamma * (d1*d2 - 1.0) / sig;

        // ── Color = ∂gamma/∂t (per day) ──────────────────────
        r.color = -r.gamma *
                   (q + (r_-q)*d1/sigSqrtT + (1.0-d1*d2)/(2.0*T))
                   / 365.0;

        return r;
    }

    // ── Put-Call Parity ───────────────────────────────────────

    /**
     * @brief Convert call price to put price via put-call parity.
     *   put = call - S*e^{-q*T} + K*e^{-r*T}
     */
    [[nodiscard]] static double callToPut(double callPrice,
                                           const BSMInput& in) noexcept {
        return callPrice
               - in.spot   * std::exp(-in.divYield * in.timeToExpiry)
               + in.strike * std::exp(-in.rate      * in.timeToExpiry);
    }

    /**
     * @brief Convert put price to call price via put-call parity.
     */
    [[nodiscard]] static double putToCall(double putPrice,
                                           const BSMInput& in) noexcept {
        return putPrice
               + in.spot   * std::exp(-in.divYield * in.timeToExpiry)
               - in.strike * std::exp(-in.rate      * in.timeToExpiry);
    }

    // ── Implied Volatility Solver ───────────────────────────────────

    /**
     * @brief Compute implied volatility using Newton-Raphson method.
     * 
     * Solves for σ such that BSM_price(σ) = market_price.
     * Uses Newton-Raphson iteration with vega as derivative.
     * 
     * @param marketPrice Observed market price of the option
     * @param in BSM input parameters (vol will be used as initial guess)
     * @param maxIterations Maximum Newton-Raphson iterations (default: 100)
     * @param tolerance Convergence tolerance (default: 1e-8)
     * @return Implied volatility or std::nullopt if convergence fails
     */
    [[nodiscard]] static std::optional<double>
    impliedVolatility(double marketPrice,
                      const BSMInput& in,
                      int maxIterations = 100,
                      double tolerance = 1e-8) noexcept {
        if (!in.isValid() || marketPrice <= 0.0) {
            return std::nullopt;
        }

        // Initial guess: use provided vol or reasonable default
        double vol = in.vol > 0.0 ? in.vol : 0.2;
        
        BSMInput input = in;
        
        for (int i = 0; i < maxIterations; ++i) {
            input.vol = vol;
            BSMResult result = compute(input);
            
            double priceError = result.price - marketPrice;
            
            // Check convergence
            if (std::abs(priceError) < tolerance) {
                return vol;
            }
            
            // Newton-Raphson update: σ_new = σ - f(σ) / f'(σ)
            // f(σ) = BSM_price(σ) - market_price
            // f'(σ) = vega
            
            if (std::abs(result.vega) < 1e-12) {
                // Vega too small, cannot continue
                return std::nullopt;
            }
            
            double volChange = priceError / result.vega;
            vol -= volChange;
            
            // Ensure volatility stays positive
            if (vol <= 0.0) {
                vol = 1e-6; // Small positive value
            }
            
            // Prevent runaway
            if (vol > 5.0) {
                return std::nullopt;
            }
        }
        
        // Failed to converge
        return std::nullopt;
    }
};

} // namespace qtl
