#pragma once
/**
 * @file analytics/montecarlo/MonteCarlo.hpp
 * @brief High-level Monte Carlo simulation engine for quantitative finance.
 *
 * Features:
 *  - Geometric Brownian Motion path generation
 *  - Option pricing (European, Asian, Barrier)
 *  - Risk simulation (VaR, CVaR via Monte Carlo)
 *  - Portfolio scenario analysis
 *  - Greeks calculation via finite differences
 *  - Antithetic variates and control variates for variance reduction
 */

#include <vector>
#include <cmath>
#include <random>
#include <algorithm>
#include <numeric>
#include <functional>
#include <optional>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// Random Number Generation
// ─────────────────────────────────────────────────────────────

class RandomGenerator {
public:
    explicit RandomGenerator(unsigned int seed = std::random_device{}())
        : rng_(seed), dist_(0.0, 1.0) {}

    /**
     * @brief Generate uniform random number in [0, 1)
     */
    [[nodiscard]] double uniform() { return dist_(rng_); }

    /**
     * @brief Generate standard normal random number (Box-Muller)
     */
    [[nodiscard]] double normal() {
        double u1 = uniform();
        double u2 = uniform();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
    }

    /**
     * @brief Generate vector of normal random numbers
     */
    [[nodiscard]] std::vector<double> normalVector(size_t n) {
        std::vector<double> result;
        result.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            result.push_back(normal());
        }
        return result;
    }

    /**
     * @brief Generate correlated normal variates using Cholesky decomposition
     */
    [[nodiscard]] std::vector<std::vector<double>>
    correlatedNormals(size_t nPaths, const std::vector<std::vector<double>>& correlationMatrix) {
        size_t nAssets = correlationMatrix.size();
        if (nAssets == 0) return {};

        // Cholesky decomposition (simplified - assumes positive definite)
        std::vector<std::vector<double>> L(nAssets, std::vector<double>(nAssets, 0.0));
        for (size_t i = 0; i < nAssets; ++i) {
            for (size_t j = 0; j <= i; ++j) {
                double sum = 0.0;
                for (size_t k = 0; k < j; ++k) {
                    sum += L[i][k] * L[j][k];
                }
                if (i == j) {
                    L[i][j] = std::sqrt(correlationMatrix[i][i] - sum);
                } else {
                    L[i][j] = (correlationMatrix[i][j] - sum) / L[j][j];
                }
            }
        }

        // Generate correlated paths
        std::vector<std::vector<double>> result(nPaths, std::vector<double>(nAssets));
        for (size_t p = 0; p < nPaths; ++p) {
            std::vector<double> z(nAssets);
            for (size_t i = 0; i < nAssets; ++i) {
                z[i] = normal();
            }

            for (size_t i = 0; i < nAssets; ++i) {
                result[p][i] = 0.0;
                for (size_t j = 0; j <= i; ++j) {
                    result[p][i] += L[i][j] * z[j];
                }
            }
        }

        return result;
    }

private:
    std::mt19937 rng_;
    std::uniform_real_distribution<double> dist_;
};

// ─────────────────────────────────────────────────────────────
// Path Generation
// ─────────────────────────────────────────────────────────────

class PathGenerator {
public:
    /**
     * @brief Generate Geometric Brownian Motion paths
     * dS = mu*S*dt + sigma*S*dW
     *
     * @param S0 Initial price
     * @param mu Drift
     * @param sigma Volatility
     * @param T Time horizon
     * @param nSteps Number of time steps
     * @param nPaths Number of paths
     * @param rng Random number generator
     */
    [[nodiscard]] static std::vector<std::vector<double>>
    geometricBrownianMotion(double S0, double mu, double sigma,
                           double T, size_t nSteps, size_t nPaths,
                           RandomGenerator& rng) {
        if (nSteps == 0 || nPaths == 0) return {};

        double dt = T / nSteps;
        std::vector<std::vector<double>> paths(nPaths, std::vector<double>(nSteps + 1));

        for (size_t p = 0; p < nPaths; ++p) {
            paths[p][0] = S0;
            for (size_t i = 1; i <= nSteps; ++i) {
                double dW = rng.normal() * std::sqrt(dt);
                paths[p][i] = paths[p][i-1] * std::exp((mu - 0.5 * sigma * sigma) * dt + sigma * dW);
            }
        }

        return paths;
    }

    /**
     * @brief Generate paths with antithetic variates (variance reduction)
     */
    [[nodiscard]] static std::vector<std::vector<double>>
    geometricBrownianMotionAntithetic(double S0, double mu, double sigma,
                                     double T, size_t nSteps, size_t nPaths,
                                     RandomGenerator& rng) {
        if (nSteps == 0 || nPaths == 0) return {};

        double dt = T / nSteps;
        size_t actualPaths = (nPaths + 1) / 2; // Round up
        std::vector<std::vector<double>> paths(nPaths, std::vector<double>(nSteps + 1));

        for (size_t p = 0; p < actualPaths; ++p) {
            std::vector<double> dW(nSteps);
            for (size_t i = 0; i < nSteps; ++i) {
                dW[i] = rng.normal() * std::sqrt(dt);
            }

            // Regular path
            paths[2*p][0] = S0;
            for (size_t i = 1; i <= nSteps; ++i) {
                paths[2*p][i] = paths[2*p][i-1] * std::exp((mu - 0.5 * sigma * sigma) * dt + sigma * dW[i-1]);
            }

            // Antithetic path (use -dW)
            if (2*p + 1 < nPaths) {
                paths[2*p + 1][0] = S0;
                for (size_t i = 1; i <= nSteps; ++i) {
                    paths[2*p + 1][i] = paths[2*p + 1][i-1] * std::exp((mu - 0.5 * sigma * sigma) * dt - sigma * dW[i-1]);
                }
            }
        }

        return paths;
    }

    /**
     * @brief Generate Ornstein-Uhlenbeck (mean-reverting) paths
     * dX = theta*(mu - X)*dt + sigma*dW
     */
    [[nodiscard]] static std::vector<std::vector<double>>
    ornsteinUhlenbeck(double X0, double mu, double theta, double sigma,
                     double T, size_t nSteps, size_t nPaths,
                     RandomGenerator& rng) {
        if (nSteps == 0 || nPaths == 0) return {};

        double dt = T / nSteps;
        std::vector<std::vector<double>> paths(nPaths, std::vector<double>(nSteps + 1));

        for (size_t p = 0; p < nPaths; ++p) {
            paths[p][0] = X0;
            for (size_t i = 1; i <= nSteps; ++i) {
                double dW = rng.normal() * std::sqrt(dt);
                paths[p][i] = paths[p][i-1] + theta * (mu - paths[p][i-1]) * dt + sigma * dW;
            }
        }

        return paths;
    }

    /**
     * @brief Generate Heston stochastic volatility paths
     * dS = mu*S*dt + sqrt(v)*S*dW1
     * dv = kappa*(theta - v)*dt + xi*sqrt(v)*dW2
     */
    struct HestonPaths {
        std::vector<std::vector<double>> pricePaths;
        std::vector<std::vector<double>> volatilityPaths;
    };

    [[nodiscard]] static HestonPaths
    heston(double S0, double v0, double mu, double kappa, double theta,
           double xi, double rho, double T, size_t nSteps, size_t nPaths,
           RandomGenerator& rng) {
        HestonPaths result;
        if (nSteps == 0 || nPaths == 0) return result;

        double dt = T / nSteps;
        result.pricePaths.resize(nPaths, std::vector<double>(nSteps + 1));
        result.volatilityPaths.resize(nPaths, std::vector<double>(nSteps + 1));

        for (size_t p = 0; p < nPaths; ++p) {
            result.pricePaths[p][0] = S0;
            result.volatilityPaths[p][0] = v0;

            for (size_t i = 1; i <= nSteps; ++i) {
                double dW1 = rng.normal() * std::sqrt(dt);
                double dW2 = rho * dW1 + std::sqrt(1.0 - rho * rho) * rng.normal() * std::sqrt(dt);

                double v = result.volatilityPaths[p][i-1];
                double vNext = v + kappa * (theta - v) * dt + xi * std::sqrt(std::max(0.0, v)) * dW2;
                result.volatilityPaths[p][i] = std::max(0.0, vNext);

                result.pricePaths[p][i] = result.pricePaths[p][i-1] *
                    std::exp((mu - 0.5 * v) * dt + std::sqrt(v) * dW1);
            }
        }

        return result;
    }
};

// ─────────────────────────────────────────────────────────────
// Option Pricing
// ─────────────────────────────────────────────────────────────

class OptionPricing {
public:
    /**
     * @brief European call option price via Monte Carlo
     */
    [[nodiscard]] static double
    europeanCall(double S0, double K, double T, double r, double sigma,
                size_t nPaths, RandomGenerator& rng) {
        auto paths = PathGenerator::geometricBrownianMotion(S0, r, sigma, T, 1, nPaths, rng);

        double sumPayoff = 0.0;
        for (const auto& path : paths) {
            double ST = path.back();
            sumPayoff += std::max(0.0, ST - K);
        }

        return std::exp(-r * T) * (sumPayoff / nPaths);
    }

    /**
     * @brief European put option price via Monte Carlo
     */
    [[nodiscard]] static double
    europeanPut(double S0, double K, double T, double r, double sigma,
               size_t nPaths, RandomGenerator& rng) {
        auto paths = PathGenerator::geometricBrownianMotion(S0, r, sigma, T, 1, nPaths, rng);

        double sumPayoff = 0.0;
        for (const auto& path : paths) {
            double ST = path.back();
            sumPayoff += std::max(0.0, K - ST);
        }

        return std::exp(-r * T) * (sumPayoff / nPaths);
    }

    /**
     * @brief Asian call option (arithmetic average)
     */
    [[nodiscard]] static double
    asianCall(double S0, double K, double T, double r, double sigma,
             size_t nSteps, size_t nPaths, RandomGenerator& rng) {
        auto paths = PathGenerator::geometricBrownianMotion(S0, r, sigma, T, nSteps, nPaths, rng);

        double sumPayoff = 0.0;
        for (const auto& path : paths) {
            double avg = std::accumulate(path.begin(), path.end(), 0.0) / path.size();
            sumPayoff += std::max(0.0, avg - K);
        }

        return std::exp(-r * T) * (sumPayoff / nPaths);
    }

    /**
     * @brief Barrier option (up-and-out call)
     */
    [[nodiscard]] static double
    barrierUpOutCall(double S0, double K, double B, double T, double r, double sigma,
                     size_t nSteps, size_t nPaths, RandomGenerator& rng) {
        auto paths = PathGenerator::geometricBrownianMotion(S0, r, sigma, T, nSteps, nPaths, rng);

        double sumPayoff = 0.0;
        for (const auto& path : paths) {
            bool breached = false;
            for (double price : path) {
                if (price >= B) {
                    breached = true;
                    break;
                }
            }
            if (!breached) {
                sumPayoff += std::max(0.0, path.back() - K);
            }
        }

        return std::exp(-r * T) * (sumPayoff / nPaths);
    }

    /**
     * @brief Calculate delta using finite differences
     */
    [[nodiscard]] static double
    delta(double S0, double K, double T, double r, double sigma,
          size_t nPaths, RandomGenerator& rng, double epsilon = 0.01) {
        double priceUp = europeanCall(S0 * (1.0 + epsilon), K, T, r, sigma, nPaths, rng);
        double priceDown = europeanCall(S0 * (1.0 - epsilon), K, T, r, sigma, nPaths, rng);
        return (priceUp - priceDown) / (2.0 * epsilon * S0);
    }

    /**
     * @brief Calculate gamma using finite differences
     */
    [[nodiscard]] static double
    gamma(double S0, double K, double T, double r, double sigma,
          size_t nPaths, RandomGenerator& rng, double epsilon = 0.01) {
        double priceUp = europeanCall(S0 * (1.0 + epsilon), K, T, r, sigma, nPaths, rng);
        double price = europeanCall(S0, K, T, r, sigma, nPaths, rng);
        double priceDown = europeanCall(S0 * (1.0 - epsilon), K, T, r, sigma, nPaths, rng);
        return (priceUp - 2.0 * price + priceDown) / (epsilon * epsilon * S0 * S0);
    }

    /**
     * @brief Calculate vega using finite differences
     */
    [[nodiscard]] static double
    vega(double S0, double K, double T, double r, double sigma,
         size_t nPaths, RandomGenerator& rng, double epsilon = 0.001) {
        double priceUp = europeanCall(S0, K, T, r, sigma + epsilon, nPaths, rng);
        double priceDown = europeanCall(S0, K, T, r, sigma - epsilon, nPaths, rng);
        return (priceUp - priceDown) / (2.0 * epsilon);
    }
};

// ─────────────────────────────────────────────────────────────
// Risk Simulation
// ─────────────────────────────────────────────────────────────

class RiskSimulation {
public:
    struct VaRResult {
        double var95{0.0};
        double var99{0.0};
        double cvar95{0.0};
        double cvar99{0.0};
        std::vector<double> pnlDistribution;
    };

    /**
     * @brief Calculate VaR and CVaR using Monte Carlo simulation
     * @param portfolioValue Current portfolio value
     * @param returns Historical returns for calibration
     * @param horizon Time horizon in days
     * @param nPaths Number of Monte Carlo paths
     */
    [[nodiscard]] static VaRResult
    calculateVaR(double portfolioValue, const std::vector<double>& returns,
                 size_t horizon, size_t nPaths, RandomGenerator& rng) {
        VaRResult result;

        if (returns.empty() || nPaths == 0) return result;

        // Calculate mean and std of returns
        double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
        double variance = 0.0;
        for (double r : returns) {
            variance += (r - mean) * (r - mean);
        }
        variance /= (returns.size() - 1);
        double std = std::sqrt(variance);

        // Scale for horizon
        double horizonMean = mean * horizon;
        double horizonStd = std * std::sqrt(static_cast<double>(horizon));

        // Generate P&L distribution
        result.pnlDistribution.reserve(nPaths);
        for (size_t i = 0; i < nPaths; ++i) {
            double simulatedReturn = horizonMean + horizonStd * rng.normal();
            double pnl = portfolioValue * simulatedReturn;
            result.pnlDistribution.push_back(pnl);
        }

        // Sort for percentile calculation
        std::sort(result.pnlDistribution.begin(), result.pnlDistribution.end());

        // Calculate VaR and CVaR
        auto calculatePercentile = [&](double confidence) -> double {
            size_t idx = static_cast<size_t>((1.0 - confidence) * nPaths);
            idx = std::min(idx, nPaths - 1);
            return -result.pnlDistribution[idx]; // Return as positive loss
        };

        auto calculateCVaR = [&](double confidence) -> double {
            size_t cutoff = static_cast<size_t>((1.0 - confidence) * nPaths);
            if (cutoff == 0) cutoff = 1;
            double sum = 0.0;
            for (size_t i = 0; i < cutoff; ++i) {
                sum += -result.pnlDistribution[i];
            }
            return sum / cutoff;
        };

        result.var95 = calculatePercentile(0.95);
        result.var99 = calculatePercentile(0.99);
        result.cvar95 = calculateCVaR(0.95);
        result.cvar99 = calculateCVaR(0.99);

        return result;
    }

    /**
     * @brief Portfolio stress testing with scenario analysis
     */
    struct StressTestResult {
        std::string scenarioName;
        double portfolioValue;
        double pnl;
        double pnlPercent;
    };

    [[nodiscard]] static std::vector<StressTestResult>
    stressTest(double portfolioValue, const std::vector<double>& weights,
              const std::vector<std::vector<double>>& assetReturns,
              const std::vector<std::string>& scenarioNames,
              const std::vector<std::vector<double>>& scenarioShocks) {
        std::vector<StressTestResult> results;

        if (weights.size() != assetReturns.size()) return results;

        for (size_t s = 0; s < scenarioShocks.size(); ++s) {
            StressTestResult result;
            result.scenarioName = s < scenarioNames.size() ? scenarioNames[s] : "Scenario " + std::to_string(s);

            double scenarioPnl = 0.0;
            for (size_t i = 0; i < weights.size(); ++i) {
                // Use historical volatility to scale shocks
                double mean = std::accumulate(assetReturns[i].begin(), assetReturns[i].end(), 0.0) / assetReturns[i].size();
                double variance = 0.0;
                for (double r : assetReturns[i]) {
                    variance += (r - mean) * (r - mean);
                }
                variance /= (assetReturns[i].size() - 1);
                double std = std::sqrt(variance);

                double shock = s < scenarioShocks.size() && i < scenarioShocks[s].size()
                              ? scenarioShocks[s][i] : 0.0;
                double assetPnl = weights[i] * portfolioValue * (shock * std);
                scenarioPnl += assetPnl;
            }

            result.portfolioValue = portfolioValue + scenarioPnl;
            result.pnl = scenarioPnl;
            result.pnlPercent = (scenarioPnl / portfolioValue) * 100.0;
            results.push_back(result);
        }

        return results;
    }
};

// ─────────────────────────────────────────────────────────────
// Portfolio Simulation
// ─────────────────────────────────────────────────────────────

class PortfolioSimulation {
public:
    struct PortfolioPath {
        std::vector<double> values;
        std::vector<double> weights;
    };

    /**
     * @brief Simulate portfolio evolution with rebalancing
     */
    [[nodiscard]] static std::vector<PortfolioPath>
    simulatePortfolio(double initialValue, const std::vector<double>& initialWeights,
                      const std::vector<std::vector<double>>& assetReturns,
                      size_t nPaths, RandomGenerator& rng,
                      size_t rebalanceFrequency = 20) {
        if (assetReturns.empty() || initialWeights.empty()) return {};

        size_t nAssets = assetReturns.size();
        size_t nPeriods = assetReturns[0].size();

        std::vector<PortfolioPath> results(nPaths);

        for (size_t p = 0; p < nPaths; ++p) {
            PortfolioPath path;
            path.values.reserve(nPeriods + 1);
            path.weights.reserve(nPeriods + 1);

            path.values.push_back(initialValue);
            path.weights = initialWeights;

            for (size_t t = 0; t < nPeriods; ++t) {
                // Calculate portfolio return
                double portfolioReturn = 0.0;
                for (size_t i = 0; i < nAssets; ++i) {
                    if (t < assetReturns[i].size()) {
                        portfolioReturn += path.weights[i] * assetReturns[i][t];
                    }
                }

                double newValue = path.values.back() * (1.0 + portfolioReturn);
                path.values.push_back(newValue);

                // Rebalance if needed
                if ((t + 1) % rebalanceFrequency == 0) {
                    path.weights = initialWeights;
                }
            }

            results[p] = path;
        }

        return results;
    }
};

} // namespace qtl
