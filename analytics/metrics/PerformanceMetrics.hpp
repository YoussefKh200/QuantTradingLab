#pragma once
/**
 * @file analytics/metrics/PerformanceMetrics.hpp
 * @brief Institutional-grade portfolio performance metrics.
 *
 * Metrics implemented
 * ───────────────────
 *  CAGR           Compound Annual Growth Rate
 *  Sharpe         Risk-adjusted return (annualised, excess over risk-free)
 *  Sortino        Downside-deviation adjusted return
 *  Calmar         CAGR / Max Drawdown
 *  Max Drawdown   Peak-to-trough decline (absolute and percentage)
 *  Win Rate       Fraction of profitable trades
 *  Expectancy     Average $ gain per trade (win_rate*avg_win - lose_rate*avg_loss)
 *  Profit Factor  Gross profit / Gross loss
 *  Avg Win/Loss   Mean P&L of winning and losing trades
 *  Volatility     Annualised standard deviation of returns
 *  VaR (95/99)    Value at Risk (parametric normal)
 *  CVaR (95/99)   Conditional VaR / Expected Shortfall
 *  Omega Ratio    Probability-weighted gains / losses above threshold
 *  Beta / Alpha   vs benchmark series (requires benchmark equity curve)
 *
 * All functions are pure — they take vectors of returns or P&L values
 * and return scalars.  No state.  Thread-safe by construction.
 *
 * Conventions
 * ───────────
 *  - Returns are simple period returns: r_t = (V_t / V_{t-1}) - 1
 *  - Annual trading days default: 252
 *  - Risk-free rate default: 0.0 (pass explicitly for Sharpe/Sortino)
 *  - Empty input → return 0.0 (never NaN/Inf exposed to caller)
 */

#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <string>
#include <sstream>
#include <iomanip>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// Constants
// ─────────────────────────────────────────────────────────────

inline constexpr double kTradingDaysPerYear  = 252.0;
inline constexpr double kTradingWeeksPerYear =  52.0;
inline constexpr double kMonthsPerYear       =  12.0;

// ─────────────────────────────────────────────────────────────
// DrawdownResult
// ─────────────────────────────────────────────────────────────

struct DrawdownResult {
    double maxDrawdown{0.0};        ///< Maximum drawdown as a fraction (e.g. -0.15 = -15%)
    double maxDrawdownAbs{0.0};     ///< Maximum drawdown in $ terms
    size_t peakIndex{0};            ///< Index of equity peak before max drawdown
    size_t troughIndex{0};          ///< Index of equity trough
    size_t recoveryIndex{0};        ///< Index where equity recovered peak (0 if not)
    int    drawdownDays{0};         ///< Duration of max drawdown in periods
    int    recoveryDays{0};         ///< Periods to recover (0 if unrecovered)
};

// ─────────────────────────────────────────────────────────────
// PerformanceMetrics — all-static utility class
// ─────────────────────────────────────────────────────────────

class PerformanceMetrics {
public:

    // ── Return series helpers ─────────────────────────────────

    /**
     * @brief Convert equity curve to period returns.
     * returns[i] = equity[i+1] / equity[i] - 1
     */
    [[nodiscard]] static std::vector<double>
    equityToReturns(const std::vector<double>& equity) {
        if (equity.size() < 2) return {};
        std::vector<double> r;
        r.reserve(equity.size() - 1);
        for (size_t i = 1; i < equity.size(); ++i) {
            if (equity[i-1] == 0.0) { r.push_back(0.0); continue; }
            r.push_back(equity[i] / equity[i-1] - 1.0);
        }
        return r;
    }

    /**
     * @brief Convert period returns to equity curve starting at initialValue.
     */
    [[nodiscard]] static std::vector<double>
    returnsToEquity(const std::vector<double>& returns,
                    double initialValue = 1.0) {
        std::vector<double> eq;
        eq.reserve(returns.size() + 1);
        eq.push_back(initialValue);
        for (double r : returns)
            eq.push_back(eq.back() * (1.0 + r));
        return eq;
    }

    // ── Core statistics ───────────────────────────────────────

    [[nodiscard]] static double mean(const std::vector<double>& v) {
        if (v.empty()) return 0.0;
        return std::accumulate(v.begin(), v.end(), 0.0) /
               static_cast<double>(v.size());
    }

    [[nodiscard]] static double variance(const std::vector<double>& v,
                                          bool ddof1 = true) {
        if (v.size() < 2) return 0.0;
        double m = mean(v);
        double sum = 0.0;
        for (double x : v) { double d = x - m; sum += d * d; }
        double denom = ddof1 ? static_cast<double>(v.size() - 1)
                             : static_cast<double>(v.size());
        return sum / denom;
    }

    [[nodiscard]] static double stddev(const std::vector<double>& v,
                                        bool ddof1 = true) {
        return std::sqrt(variance(v, ddof1));
    }

    [[nodiscard]] static double downsideDeviation(
            const std::vector<double>& returns,
            double targetReturn = 0.0) {
        if (returns.empty()) return 0.0;
        double sum = 0.0;
        int    cnt = 0;
        for (double r : returns) {
            double diff = std::min(r - targetReturn, 0.0);
            sum += diff * diff;
            ++cnt;
        }
        return cnt > 0 ? std::sqrt(sum / cnt) : 0.0;
    }

    // ── Primary metrics ───────────────────────────────────────

    /**
     * @brief CAGR from equity curve.
     * CAGR = (end / start)^(1/years) - 1
     *
     * @param equity        Equity curve (must have >= 2 points)
     * @param periodsPerYear Trading periods per year (252=daily, 52=weekly, 12=monthly)
     */
    [[nodiscard]] static double cagr(const std::vector<double>& equity,
                                      double periodsPerYear = kTradingDaysPerYear) {
        if (equity.size() < 2 || equity.front() <= 0.0) return 0.0;
        double years = static_cast<double>(equity.size() - 1) / periodsPerYear;
        if (years <= 0.0) return 0.0;
        return std::pow(equity.back() / equity.front(), 1.0 / years) - 1.0;
    }

    /**
     * @brief Annualised Sharpe ratio.
     * Sharpe = (mean(r) - rf) / std(r) * sqrt(periodsPerYear)
     *
     * @param returns       Period returns
     * @param riskFreeRate  Annual risk-free rate (e.g. 0.05 = 5%)
     * @param periodsPerYear Periods per year for annualisation
     */
    [[nodiscard]] static double sharpe(const std::vector<double>& returns,
                                        double riskFreeRate   = 0.0,
                                        double periodsPerYear = kTradingDaysPerYear) {
        if (returns.size() < 2) return 0.0;
        double rfPeriod = riskFreeRate / periodsPerYear;
        double excessMean = mean(returns) - rfPeriod;
        double sd = stddev(returns);
        if (sd == 0.0) return 0.0;
        return excessMean / sd * std::sqrt(periodsPerYear);
    }

    /**
     * @brief Annualised Sortino ratio.
     * Sortino = (mean(r) - rf) / downside_std(r) * sqrt(periodsPerYear)
     */
    [[nodiscard]] static double sortino(const std::vector<double>& returns,
                                         double riskFreeRate   = 0.0,
                                         double periodsPerYear = kTradingDaysPerYear) {
        if (returns.empty()) return 0.0;
        double rfPeriod  = riskFreeRate / periodsPerYear;
        double excessMean = mean(returns) - rfPeriod;
        double dd = downsideDeviation(returns, rfPeriod);
        if (dd == 0.0) return 0.0;
        return excessMean / dd * std::sqrt(periodsPerYear);
    }

    /**
     * @brief Maximum drawdown analysis from equity curve.
     */
    [[nodiscard]] static DrawdownResult maxDrawdown(
            const std::vector<double>& equity) {
        DrawdownResult result;
        if (equity.size() < 2) return result;

        double peak     = equity[0];
        double maxDD    = 0.0;
        size_t peakIdx  = 0;
        size_t troughIdx= 0;
        size_t curPeak  = 0;

        for (size_t i = 1; i < equity.size(); ++i) {
            if (equity[i] > peak) {
                peak    = equity[i];
                curPeak = i;
            }
            double dd = (equity[i] - peak) / peak;
            if (dd < maxDD) {
                maxDD    = dd;
                peakIdx  = curPeak;
                troughIdx= i;
            }
        }

        result.maxDrawdown    = maxDD;
        result.maxDrawdownAbs = equity[peakIdx] * maxDD; // negative
        result.peakIndex      = peakIdx;
        result.troughIndex    = troughIdx;
        result.drawdownDays   = static_cast<int>(troughIdx - peakIdx);

        // Find recovery
        double peakVal = equity[peakIdx];
        for (size_t i = troughIdx; i < equity.size(); ++i) {
            if (equity[i] >= peakVal) {
                result.recoveryIndex = i;
                result.recoveryDays  = static_cast<int>(i - troughIdx);
                break;
            }
        }
        return result;
    }

    /**
     * @brief Calmar ratio = CAGR / |Max Drawdown|
     */
    [[nodiscard]] static double calmar(const std::vector<double>& equity,
                                        double periodsPerYear = kTradingDaysPerYear) {
        double dd = maxDrawdown(equity).maxDrawdown;
        if (dd == 0.0) return 0.0;
        return cagr(equity, periodsPerYear) / std::abs(dd);
    }

    // ── Trade statistics ──────────────────────────────────────

    /**
     * @brief Win rate from a series of per-trade P&L values.
     */
    [[nodiscard]] static double winRate(const std::vector<double>& tradePnl) {
        if (tradePnl.empty()) return 0.0;
        int wins = 0;
        for (double p : tradePnl) if (p > 0.0) ++wins;
        return static_cast<double>(wins) / static_cast<double>(tradePnl.size());
    }

    /**
     * @brief Expectancy = win_rate * avg_win + loss_rate * avg_loss
     */
    [[nodiscard]] static double expectancy(const std::vector<double>& tradePnl) {
        if (tradePnl.empty()) return 0.0;
        double sumWin = 0.0, sumLoss = 0.0;
        int    cntWin = 0,   cntLoss = 0;
        for (double p : tradePnl) {
            if (p > 0.0) { sumWin  += p; ++cntWin;  }
            else          { sumLoss += p; ++cntLoss; }
        }
        double wr = static_cast<double>(cntWin) /
                    static_cast<double>(tradePnl.size());
        double lr = 1.0 - wr;
        double avgWin  = cntWin  > 0 ? sumWin  / cntWin  : 0.0;
        double avgLoss = cntLoss > 0 ? sumLoss / cntLoss : 0.0;
        return wr * avgWin + lr * avgLoss;
    }

    /**
     * @brief Profit factor = gross_profit / |gross_loss|
     */
    [[nodiscard]] static double profitFactor(
            const std::vector<double>& tradePnl) {
        double grossProfit = 0.0, grossLoss = 0.0;
        for (double p : tradePnl) {
            if (p > 0.0) grossProfit += p;
            else          grossLoss  += std::abs(p);
        }
        return grossLoss > 0.0 ? grossProfit / grossLoss : 0.0;
    }

    /**
     * @brief Average winning trade P&L.
     */
    [[nodiscard]] static double avgWin(const std::vector<double>& tradePnl) {
        double sum = 0.0; int cnt = 0;
        for (double p : tradePnl) if (p > 0.0) { sum += p; ++cnt; }
        return cnt > 0 ? sum / cnt : 0.0;
    }

    /**
     * @brief Average losing trade P&L (negative value).
     */
    [[nodiscard]] static double avgLoss(const std::vector<double>& tradePnl) {
        double sum = 0.0; int cnt = 0;
        for (double p : tradePnl) if (p <= 0.0) { sum += p; ++cnt; }
        return cnt > 0 ? sum / cnt : 0.0;
    }

    /**
     * @brief Annualised volatility from period returns.
     */
    [[nodiscard]] static double annualisedVol(
            const std::vector<double>& returns,
            double periodsPerYear = kTradingDaysPerYear) {
        return stddev(returns) * std::sqrt(periodsPerYear);
    }

    // ── Risk metrics ──────────────────────────────────────────

    /**
     * @brief Parametric Value at Risk (normal distribution).
     * VaR_alpha = -(mean - z_alpha * std)
     *
     * @param returns    Period returns
     * @param confidence Confidence level (0.95 or 0.99)
     */
    [[nodiscard]] static double varParametric(
            const std::vector<double>& returns,
            double confidence = 0.95) {
        if (returns.empty()) return 0.0;
        // z-scores: 1.645 for 95%, 2.326 for 99%
        double z = (confidence >= 0.99) ? 2.326 : 1.645;
        double m = mean(returns);
        double s = stddev(returns);
        return -(m - z * s);
    }

    /**
     * @brief Historical VaR — percentile of actual return distribution.
     */
    [[nodiscard]] static double varHistorical(
            const std::vector<double>& returns,
            double confidence = 0.95) {
        if (returns.empty()) return 0.0;
        std::vector<double> sorted = returns;
        std::sort(sorted.begin(), sorted.end());
        size_t idx = static_cast<size_t>(
            (1.0 - confidence) * static_cast<double>(sorted.size()));
        idx = std::min(idx, sorted.size() - 1);
        return -sorted[idx];
    }

    /**
     * @brief Conditional VaR (Expected Shortfall) — mean of worst returns.
     */
    [[nodiscard]] static double cvar(
            const std::vector<double>& returns,
            double confidence = 0.95) {
        if (returns.empty()) return 0.0;
        std::vector<double> sorted = returns;
        std::sort(sorted.begin(), sorted.end());
        size_t cutoff = static_cast<size_t>(
            (1.0 - confidence) * static_cast<double>(sorted.size()));
        if (cutoff == 0) cutoff = 1;
        double sum = 0.0;
        for (size_t i = 0; i < cutoff; ++i) sum += sorted[i];
        return -(sum / static_cast<double>(cutoff));
    }

    /**
     * @brief Omega ratio — P(r > threshold) weighted gains / losses.
     */
    [[nodiscard]] static double omega(const std::vector<double>& returns,
                                       double threshold = 0.0) {
        if (returns.empty()) return 0.0;
        double gains = 0.0, losses = 0.0;
        for (double r : returns) {
            double diff = r - threshold;
            if (diff > 0.0) gains  += diff;
            else             losses -= diff;
        }
        return losses > 0.0 ? gains / losses : 0.0;
    }

    /**
     * @brief Beta and alpha vs benchmark returns.
     * beta  = cov(strat, bench) / var(bench)
     * alpha = mean(strat) - beta * mean(bench)  (per-period, not annualised)
     */
    struct BetaAlpha { double beta{0.0}; double alpha{0.0}; };

    [[nodiscard]] static BetaAlpha betaAlpha(
            const std::vector<double>& stratReturns,
            const std::vector<double>& benchReturns) {
        size_t n = std::min(stratReturns.size(), benchReturns.size());
        if (n < 2) return {};
        double ms = mean({stratReturns.begin(), stratReturns.begin() + n});
        double mb = mean({benchReturns.begin(),  benchReturns.begin()  + n});
        double cov = 0.0, varB = 0.0;
        for (size_t i = 0; i < n; ++i) {
            cov  += (stratReturns[i] - ms) * (benchReturns[i] - mb);
            varB += (benchReturns[i] - mb) * (benchReturns[i] - mb);
        }
        cov  /= (n - 1);
        varB /= (n - 1);
        BetaAlpha ba;
        ba.beta  = varB > 0.0 ? cov / varB : 0.0;
        ba.alpha = ms - ba.beta * mb;
        return ba;
    }

    // ── Comprehensive summary ─────────────────────────────────

    struct Summary {
        // Return metrics
        double totalReturn{0.0};
        double cagr{0.0};
        double annualisedVol{0.0};
        // Risk-adjusted
        double sharpe{0.0};
        double sortino{0.0};
        double calmar{0.0};
        double omega{0.0};
        // Drawdown
        DrawdownResult drawdown;
        // Trade stats
        double winRate{0.0};
        double expectancy{0.0};
        double profitFactor{0.0};
        double avgWin{0.0};
        double avgLoss{0.0};
        int    totalTrades{0};
        int    winningTrades{0};
        int    losingTrades{0};
        // Risk
        double var95{0.0};
        double var99{0.0};
        double cvar95{0.0};
        double cvar99{0.0};

        [[nodiscard]] std::string toString() const {
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(4);
            oss << "══════════════ Performance Summary ══════════════\n";
            oss << "  Total Return   : " << (totalReturn * 100.0)   << "%\n";
            oss << "  CAGR           : " << (cagr         * 100.0)  << "%\n";
            oss << "  Ann. Volatility: " << (annualisedVol * 100.0) << "%\n";
            oss << "  Sharpe Ratio   : " << sharpe        << "\n";
            oss << "  Sortino Ratio  : " << sortino       << "\n";
            oss << "  Calmar Ratio   : " << calmar        << "\n";
            oss << "  Omega Ratio    : " << omega         << "\n";
            oss << "  Max Drawdown   : " << (drawdown.maxDrawdown * 100.0) << "%\n";
            oss << "  DD Duration    : " << drawdown.drawdownDays << " periods\n";
            oss << "  DD Recovery    : " << (drawdown.recoveryDays > 0
                                             ? std::to_string(drawdown.recoveryDays) + " periods"
                                             : "unrecovered") << "\n";
            oss << "─────────────── Trade Statistics ────────────────\n";
            oss << "  Total Trades   : " << totalTrades   << "\n";
            oss << "  Win Rate       : " << (winRate * 100.0)    << "%\n";
            oss << "  Expectancy     : " << expectancy    << "\n";
            oss << "  Profit Factor  : " << profitFactor  << "\n";
            oss << "  Avg Win        : " << avgWin        << "\n";
            oss << "  Avg Loss       : " << avgLoss       << "\n";
            oss << "─────────────── Risk Metrics ────────────────────\n";
            oss << "  VaR  95%       : " << (var95  * 100.0) << "%\n";
            oss << "  VaR  99%       : " << (var99  * 100.0) << "%\n";
            oss << "  CVaR 95%       : " << (cvar95 * 100.0) << "%\n";
            oss << "  CVaR 99%       : " << (cvar99 * 100.0) << "%\n";
            oss << "═════════════════════════════════════════════════\n";
            return oss.str();
        }
    };

    [[nodiscard]] static Summary computeSummary(
            const std::vector<double>& equity,
            const std::vector<double>& tradePnl  = {},
            double riskFreeRate   = 0.0,
            double periodsPerYear = kTradingDaysPerYear) {
        Summary s;
        if (equity.size() < 2) return s;

        auto returns = equityToReturns(equity);

        // Return metrics
        s.totalReturn   = equity.back() / equity.front() - 1.0;
        s.cagr          = PerformanceMetrics::cagr(equity, periodsPerYear);
        s.annualisedVol = annualisedVol(returns, periodsPerYear);

        // Risk-adjusted
        s.sharpe  = PerformanceMetrics::sharpe (returns, riskFreeRate, periodsPerYear);
        s.sortino = PerformanceMetrics::sortino(returns, riskFreeRate, periodsPerYear);
        s.calmar  = PerformanceMetrics::calmar (equity, periodsPerYear);
        s.omega   = PerformanceMetrics::omega  (returns);

        // Drawdown
        s.drawdown = maxDrawdown(equity);

        // Risk
        s.var95  = varHistorical(returns, 0.95);
        s.var99  = varHistorical(returns, 0.99);
        s.cvar95 = cvar(returns, 0.95);
        s.cvar99 = cvar(returns, 0.99);

        // Trade stats
        if (!tradePnl.empty()) {
            s.totalTrades  = static_cast<int>(tradePnl.size());
            s.winRate      = PerformanceMetrics::winRate(tradePnl);
            s.expectancy   = PerformanceMetrics::expectancy(tradePnl);
            s.profitFactor = PerformanceMetrics::profitFactor(tradePnl);
            s.avgWin       = PerformanceMetrics::avgWin(tradePnl);
            s.avgLoss      = PerformanceMetrics::avgLoss(tradePnl);
            for (double p : tradePnl) {
                if (p > 0.0) ++s.winningTrades;
                else          ++s.losingTrades;
            }
        }
        return s;
    }
};

} // namespace qtl
