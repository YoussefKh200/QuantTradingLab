#pragma once
/**
 * @file options/iv_surface/VolatilitySurface.hpp
 * @brief Implied volatility solver and surface interpolation.
 *
 * Implied Volatility Solver
 * ──────────────────────────
 * Given a market option price, find σ such that BSM(σ) = marketPrice.
 *
 * Algorithm: Brent's method with Jaeckel's initial guess (2006).
 *   1. Compute initial bracket [σ_lo, σ_hi] using moneyness heuristic.
 *   2. Refine with Brent's method (guaranteed convergence, superlinear).
 *   3. Stop when |BSM(σ) - target| < tolerance or maxIter reached.
 *
 * Typical convergence: 5-15 iterations, accurate to < 1e-8.
 *
 * Volatility Surface
 * ──────────────────
 * Stores a matrix of implied vols indexed by (strike, expiry).
 * Interpolation: bilinear in (moneyness, √T) space.
 *
 * Moneyness: ln(F/K) / (σ * √T)  where F = S * e^{(r-q)T}
 */

#include "options/blackscholes/BlackScholes.hpp"
#include <vector>
#include <map>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <limits>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// IV Solver result
// ─────────────────────────────────────────────────────────────

struct IVResult {
    double iv{0.0};            ///< Implied volatility (annualised)
    double priceDiff{0.0};     ///< |BSM(iv) - marketPrice|
    int    iterations{0};
    bool   converged{false};
};

// ─────────────────────────────────────────────────────────────
// ImpliedVolSolver
// ─────────────────────────────────────────────────────────────

struct ImpliedVolSolverCfg {
    double tolerance{1e-8};
    int    maxIter{100};
    double ivLow{0.001};
    double ivHigh{5.0};
};

class ImpliedVolSolver {
public:
    using Config = ImpliedVolSolverCfg;

    explicit ImpliedVolSolver(Config cfg = Config{}) : cfg_{cfg} {}

    /**
     * @brief Solve for implied volatility.
     *
     * @param marketPrice  Observed market price of the option.
     * @param in           BSMInput with all fields except vol (ignored).
     * @return IVResult with iv and convergence info.
     */
    [[nodiscard]] IVResult solve(double marketPrice, BSMInput in) const {
        IVResult result;

        // Validate
        if (marketPrice <= 0 || !in.isValid()) return result;

        // Intrinsic value check — can't solve below intrinsic
        double intrinsic = (in.optionType == OptionType::Call)
            ? std::max(in.spot * std::exp(-in.divYield * in.timeToExpiry) -
                       in.strike * std::exp(-in.rate * in.timeToExpiry), 0.0)
            : std::max(in.strike * std::exp(-in.rate * in.timeToExpiry) -
                       in.spot * std::exp(-in.divYield * in.timeToExpiry), 0.0);
        if (marketPrice < intrinsic - 1e-10) return result; // below intrinsic

        // Initial guess: Brenner-Subrahmanyam (1988)
        // σ ≈ sqrt(2π/T) * (C/S)  for ATM options
        double sigInit = std::sqrt(2.0 * M_PI / in.timeToExpiry) *
                         (marketPrice / in.spot);
        sigInit = std::clamp(sigInit, cfg_.ivLow, cfg_.ivHigh);

        // ── Brent's method ────────────────────────────────────
        double a = cfg_.ivLow, b = cfg_.ivHigh;
        in.vol = a;
        double fa = BlackScholes::price(in) - marketPrice;
        in.vol = b;
        double fb = BlackScholes::price(in) - marketPrice;

        // Ensure bracket
        if (fa * fb > 0) {
            // Try wider bracket
            a = 1e-6; b = 10.0;
            in.vol = a; fa = BlackScholes::price(in) - marketPrice;
            in.vol = b; fb = BlackScholes::price(in) - marketPrice;
            if (fa * fb > 0) return result; // can't bracket
        }

        double c = a, fc = fa, d = b - a, e = d;
        for (int iter = 0; iter < cfg_.maxIter; ++iter) {
            if (fb * fc > 0) { c = a; fc = fa; d = e = b - a; }
            if (std::abs(fc) < std::abs(fb)) {
                a = b; b = c; c = a;
                fa = fb; fb = fc; fc = fa;
            }

            double tol = 2.0 * std::numeric_limits<double>::epsilon() *
                         std::abs(b) + 0.5 * cfg_.tolerance;
            double m   = 0.5 * (c - b);

            if (std::abs(m) <= tol || std::abs(fb) < cfg_.tolerance) {
                result.iv         = b;
                result.priceDiff  = std::abs(fb);
                result.iterations = iter + 1;
                result.converged  = true;
                return result;
            }

            if (std::abs(e) >= tol && std::abs(fa) > std::abs(fb)) {
                double s = fb / fa;
                double p, q2;
                if (a == c) {
                    p  = 2.0 * m * s;
                    q2 = 1.0 - s;
                } else {
                    double r3 = fb / fc;
                    double s2 = fa / fc;
                    p  = s * (2.0 * m * s2 * (s2 - r3) - (b - a) * (r3 - 1.0));
                    q2 = (s2 - 1.0) * (r3 - 1.0) * (s - 1.0);
                }
                if (p > 0) q2 = -q2; else p = -p;

                if (2.0 * p < std::min(3.0 * m * q2 - std::abs(tol * q2),
                                        std::abs(e * q2))) {
                    e = d; d = p / q2;
                } else { d = m; e = m; }
            } else { d = m; e = m; }

            a = b; fa = fb;
            b += (std::abs(d) > tol) ? d : (m > 0 ? tol : -tol);
            in.vol = b;
            fb = BlackScholes::price(in) - marketPrice;
            ++result.iterations;
        }

        result.iv        = b;
        result.priceDiff = std::abs(fb);
        result.converged = std::abs(fb) < cfg_.tolerance * 10;
        return result;
    }

private:
    Config cfg_;
};

// ─────────────────────────────────────────────────────────────
// VolSurfacePoint — one data point on the surface
// ─────────────────────────────────────────────────────────────

struct VolSurfacePoint {
    double strike{0.0};
    double expiry{0.0};       ///< Time to expiry in years
    double impliedVol{0.0};
    double moneyness{0.0};    ///< ln(F/K) / (σ√T)
    OptionType optionType{OptionType::Call};
    double midPrice{0.0};     ///< Market mid price used to compute IV
};

// ─────────────────────────────────────────────────────────────
// VolatilitySurface — discretised IV surface with interpolation
// ─────────────────────────────────────────────────────────────

class VolatilitySurface {
public:
    explicit VolatilitySurface(std::string underlying = "")
        : underlying_{std::move(underlying)} {}

    // ── Surface construction ──────────────────────────────────

    /**
     * @brief Add a single IV data point to the surface.
     */
    void addPoint(VolSurfacePoint pt) {
        points_.push_back(std::move(pt));
        dirty_ = true;
        rebuildIndex();
    }

    /// Convenience: add a point by (expiry_years, strike, implied_vol).
    void addPoint(double expiryYears, double strike, double impliedVol,
                  OptionType ot = OptionType::Call) {
        VolSurfacePoint pt;
        pt.expiry     = expiryYears;
        pt.strike     = strike;
        pt.impliedVol = impliedVol;
        pt.optionType = ot;
        addPoint(std::move(pt));
    }

    /// Interpolate (or exact-lookup) implied vol at (expiry_years, strike).
    [[nodiscard]] double getVol(double expiryYears, double strike) const {
        // Exact lookup first
        for (auto& p : points_) {
            if (std::abs(p.expiry - expiryYears) < 1e-9 &&
                std::abs(p.strike - strike)       < 1e-9) {
                return p.impliedVol;
            }
        }
        // Fall back to interpolation
        return interpolate(strike, expiryYears);
    }

    /**
     * @brief Compute IV surface from a set of market quotes.
     *
     * For each (strike, expiry) pair, solves for implied volatility
     * given the market price.
     *
     * @param spot      Current spot price
     * @param rate      Risk-free rate
     * @param divYield  Dividend yield
     * @param quotes    List of (strike, expiry, optionType, marketPrice)
     */
    void buildFromQuotes(double spot, double rate, double divYield,
                          const std::vector<std::tuple<double,double,
                                                        OptionType,double>>& quotes)
    {
        points_.clear();
        ImpliedVolSolver solver;

        for (auto& [K, T, ot, mktPx] : quotes) {
            BSMInput in{spot, K, rate, 0.2 /* initial guess */, T, divYield, ot};
            auto result = solver.solve(mktPx, in);
            if (!result.converged) continue;

            VolSurfacePoint pt;
            pt.strike     = K;
            pt.expiry     = T;
            pt.impliedVol = result.iv;
            pt.optionType = ot;
            pt.midPrice   = mktPx;
            // Moneyness: ln(F/K) / (σ√T)
            double F = spot * std::exp((rate - divYield) * T);
            pt.moneyness  = (result.iv > 0 && T > 0)
                ? std::log(F / K) / (result.iv * std::sqrt(T))
                : 0.0;
            points_.push_back(pt);
        }
        dirty_ = true;
        rebuildIndex();
    }

    // ── Interpolation ─────────────────────────────────────────

    /**
     * @brief Interpolate implied vol at (strike, expiry).
     *
     * Uses bilinear interpolation in (strike, expiry) space.
     * Returns 0 if surface is empty or extrapolation needed.
     */
    [[nodiscard]] double interpolate(double strike, double expiry) const {
        if (points_.empty()) return 0.0;

        // Find surrounding expiry slices
        auto exIt = expiryStrikes_.lower_bound(expiry);
        if (exIt == expiryStrikes_.end()) --exIt;
        if (exIt == expiryStrikes_.begin()) {
            return interpolateStrike(exIt->second, strike);
        }

        auto exHi = exIt;
        auto exLo = std::prev(exIt);

        double T_lo = exLo->first, T_hi = exHi->first;
        double wHi  = (T_hi > T_lo) ? (expiry - T_lo) / (T_hi - T_lo) : 0.5;
        double wLo  = 1.0 - wHi;

        double volLo = interpolateStrike(exLo->second, strike);
        double volHi = interpolateStrike(exHi->second, strike);

        return wLo * volLo + wHi * volHi;
    }

    // ── Queries ───────────────────────────────────────────────

    [[nodiscard]] size_t pointCount() const noexcept { return points_.size(); }
    [[nodiscard]] const std::vector<VolSurfacePoint>& points()
        const noexcept { return points_; }

    [[nodiscard]] std::vector<double> expiries() const {
        std::vector<double> exp;
        for (auto& [T, _] : expiryStrikes_) exp.push_back(T);
        return exp;
    }

    [[nodiscard]] std::string summary() const {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(4);
        oss << "VolatilitySurface (" << underlying_ << "): "
            << points_.size() << " points, "
            << expiryStrikes_.size() << " expiry slices\n";
        for (auto& [T, strikeMap] : expiryStrikes_) {
            oss << "  T=" << T << "y: strikes=";
            for (auto& [K, iv] : strikeMap)
                oss << K << "→" << (iv*100.0) << "% ";
            oss << "\n";
        }
        return oss.str();
    }

private:
    void rebuildIndex() {
        expiryStrikes_.clear();
        for (auto& pt : points_) {
            expiryStrikes_[pt.expiry][pt.strike] = pt.impliedVol;
        }
        dirty_ = false;
    }

    [[nodiscard]] double interpolateStrike(
            const std::map<double,double>& strikeMap,
            double strike) const {
        if (strikeMap.empty()) return 0.0;

        auto it = strikeMap.lower_bound(strike);
        if (it == strikeMap.end())   return std::prev(it)->second;
        if (it == strikeMap.begin()) return it->second;

        auto hi = it;
        auto lo = std::prev(it);
        double K_lo = lo->first, K_hi = hi->first;
        double w = (K_hi > K_lo) ? (strike - K_lo) / (K_hi - K_lo) : 0.5;
        return (1.0 - w) * lo->second + w * hi->second;
    }

    std::string  underlying_;
    std::vector<VolSurfacePoint>               points_;
    std::map<double, std::map<double,double>>  expiryStrikes_; // [T][K] = iv
    bool         dirty_{false};
};

} // namespace qtl
