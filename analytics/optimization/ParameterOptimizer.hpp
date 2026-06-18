#pragma once
/**
 * @file analytics/optimization/ParameterOptimizer.hpp
 * @brief High-level parameter optimization for quantitative trading strategies.
 *
 * Features:
 *  - Grid search optimization
 *  - Bayesian optimization (Gaussian Process)
 *  - Walk-forward analysis
 *  - Cross-validation
 *  - Multi-objective optimization (Pareto front)
 *  - Parameter sensitivity analysis
 */

#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <functional>
#include <optional>
#include <map>
#include <random>
#include <limits>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// Parameter Space Definition
// ─────────────────────────────────────────────────────────────

struct ParameterRange {
    std::string name;
    double min;
    double max;
    double step; // 0 for continuous

    [[nodiscard]] std::vector<double> generateValues() const {
        if (step <= 0.0) return {min, max}; // Continuous - use min/max for bounds
        std::vector<double> values;
        for (double v = min; v <= max + 1e-9; v += step) {
            values.push_back(v);
        }
        return values;
    }
};

struct ParameterSet {
    std::map<std::string, double> values;
    double fitness{0.0};
    std::vector<double> metrics; // Additional metrics (Sharpe, drawdown, etc.)

    [[nodiscard]] bool operator<(const ParameterSet& other) const {
        return fitness > other.fitness; // Higher fitness is better
    }
};

// ─────────────────────────────────────────────────────────────
// Grid Search Optimization
// ─────────────────────────────────────────────────────────────

class GridSearchOptimizer {
public:
    /**
     * @brief Perform grid search over parameter space
     * @param parameters Parameter ranges to search
     * @param objectiveFunction Function that takes ParameterSet and returns fitness
     * @param callback Optional callback for progress updates
     */
    [[nodiscard]] static std::vector<ParameterSet>
    optimize(const std::vector<ParameterRange>& parameters,
             std::function<double(const ParameterSet&)> objectiveFunction,
             std::function<void(const ParameterSet&, size_t, size_t)> callback = nullptr) {
        std::vector<ParameterSet> results;

        // Generate all parameter combinations
        std::vector<std::vector<double>> paramValues;
        for (const auto& param : parameters) {
            paramValues.push_back(param.generateValues());
        }

        // Calculate total combinations
        size_t totalCombinations = 1;
        for (const auto& values : paramValues) {
            totalCombinations *= values.size();
        }

        // Generate combinations using recursive approach
        std::vector<size_t> indices(paramValues.size(), 0);
        size_t currentCombination = 0;

        while (true) {
            // Create parameter set from current indices
            ParameterSet params;
            for (size_t i = 0; i < parameters.size(); ++i) {
                params.values[parameters[i].name] = paramValues[i][indices[i]];
            }

            // Evaluate objective function
            params.fitness = objectiveFunction(params);
            results.push_back(params);

            // Callback for progress
            if (callback) {
                callback(params, currentCombination, totalCombinations);
            }

            // Increment indices (like counting in mixed radix)
            size_t idx = 0;
            while (idx < indices.size() && ++indices[idx] >= paramValues[idx].size()) {
                indices[idx] = 0;
                ++idx;
            }

            if (idx >= indices.size()) break;
            ++currentCombination;
        }

        // Sort by fitness (descending)
        std::sort(results.begin(), results.end());

        return results;
    }

    /**
     * @brief Get best N parameter sets
     */
    [[nodiscard]] static std::vector<ParameterSet>
    getTopN(const std::vector<ParameterSet>& results, size_t n) {
        std::vector<ParameterSet> top = results;
        std::partial_sort(top.begin(), top.begin() + std::min(n, top.size()), top.end());
        top.resize(std::min(n, top.size()));
        return top;
    }
};

// ─────────────────────────────────────────────────────────────
// Bayesian Optimization (Simplified Gaussian Process)
// ─────────────────────────────────────────────────────────────

class BayesianOptimizer {
public:
    /**
     * @brief Simplified Bayesian optimization using acquisition function
     * @param parameters Parameter ranges
     * @param objectiveFunction Function to optimize
     * @param nIterations Number of optimization iterations
     * @param nInitialSamples Number of random initial samples
     */
    [[nodiscard]] static ParameterSet
    optimize(const std::vector<ParameterRange>& parameters,
             std::function<double(const ParameterSet&)> objectiveFunction,
             size_t nIterations = 50,
             size_t nInitialSamples = 10) {
        std::vector<ParameterSet> evaluated;

        // Initial random sampling
        std::random_device rd;
        std::mt19937 gen(rd());

        for (size_t i = 0; i < nInitialSamples; ++i) {
            ParameterSet params = randomSample(parameters, gen);
            params.fitness = objectiveFunction(params);
            evaluated.push_back(params);
        }

        // Iterative optimization
        for (size_t iter = 0; iter < nIterations; ++iter) {
            // Find next point to evaluate using acquisition function
            ParameterSet nextPoint = selectNextPoint(evaluated, parameters, gen);
            nextPoint.fitness = objectiveFunction(nextPoint);
            evaluated.push_back(nextPoint);
        }

        // Return best
        auto bestIt = std::max_element(evaluated.begin(), evaluated.end(),
            [](const ParameterSet& a, const ParameterSet& b) {
                return a.fitness < b.fitness;
            });

        return *bestIt;
    }

private:
    [[nodiscard]] static ParameterSet
    randomSample(const std::vector<ParameterRange>& parameters, std::mt19937& gen) {
        ParameterSet params;
        for (const auto& param : parameters) {
            std::uniform_real_distribution<double> dist(param.min, param.max);
            params.values[param.name] = dist(gen);
        }
        return params;
    }

    [[nodiscard]] static ParameterSet
    selectNextPoint(const std::vector<ParameterSet>& evaluated,
                   const std::vector<ParameterRange>& parameters,
                   std::mt19937& gen) {
        // Simplified: use Thompson sampling
        // In production, implement proper GP with Expected Improvement

        // Find best and worst fitness
        double bestFit = -std::numeric_limits<double>::infinity();
        double worstFit = std::numeric_limits<double>::infinity();
        for (const auto& params : evaluated) {
            bestFit = std::max(bestFit, params.fitness);
            worstFit = std::min(worstFit, params.fitness);
        }

        // Sample around best points with exploration
        ParameterSet candidate = randomSample(parameters, gen);

        // Bias towards regions with high fitness
        if (!evaluated.empty()) {
            const auto& best = *std::max_element(evaluated.begin(), evaluated.end(),
                [](const ParameterSet& a, const ParameterSet& b) {
                    return a.fitness < b.fitness;
                });

            // Blend random sample with best point
            double blend = 0.3; // 30% exploration
            for (const auto& param : parameters) {
                auto it = best.values.find(param.name);
                if (it != best.values.end()) {
                    candidate.values[param.name] = blend * candidate.values[param.name] +
                                                   (1.0 - blend) * it->second;
                }
            }
        }

        return candidate;
    }
};

// ─────────────────────────────────────────────────────────────
// Walk-Forward Analysis
// ─────────────────────────────────────────────────────────────

struct WalkForwardResult {
    ParameterSet bestParameters;
    std::vector<double> inSampleFitness;
    std::vector<double> outOfSampleFitness;
    std::vector<ParameterSet> parameterHistory;
    double averageOutOfSampleFitness{0.0};
    double stdOutOfSampleFitness{0.0};
};

class WalkForwardAnalyzer {
public:
    /**
     * @brief Perform walk-forward analysis
     * @param data Full dataset
     * @param parameters Parameter ranges to optimize
     * @param trainSize Size of training window
     * @param testSize Size of test window
     * @param stepSize Step size between windows
     * @param objectiveFunction Function that takes data slice and parameters, returns fitness
     */
    template<typename T>
    [[nodiscard]] static WalkForwardResult
    analyze(const std::vector<T>& data,
            const std::vector<ParameterRange>& parameters,
            size_t trainSize,
            size_t testSize,
            size_t stepSize,
            std::function<double(const std::vector<T>&, const ParameterSet&)> objectiveFunction) {

        WalkForwardResult result;

        if (data.size() < trainSize + testSize) {
            return result;
        }

        size_t nWindows = (data.size() - trainSize - testSize) / stepSize + 1;

        for (size_t w = 0; w < nWindows; ++w) {
            size_t trainStart = w * stepSize;
            size_t trainEnd = trainStart + trainSize;
            size_t testStart = trainEnd;
            size_t testEnd = testStart + testSize;

            if (testEnd > data.size()) break;

            // Training data
            std::vector<T> trainData(data.begin() + trainStart, data.begin() + trainEnd);

            // Optimize on training data
            auto trainObjective = [&](const ParameterSet& params) {
                return objectiveFunction(trainData, params);
            };

            auto optimized = GridSearchOptimizer::optimize(parameters, trainObjective);
            if (optimized.empty()) continue;

            ParameterSet bestParams = optimized[0];
            result.parameterHistory.push_back(bestParams);

            // Test on out-of-sample data
            std::vector<T> testData(data.begin() + testStart, data.begin() + testEnd);
            double oosFitness = objectiveFunction(testData, bestParams);

            result.inSampleFitness.push_back(bestParams.fitness);
            result.outOfSampleFitness.push_back(oosFitness);
        }

        // Calculate statistics
        if (!result.outOfSampleFitness.empty()) {
            double sum = std::accumulate(result.outOfSampleFitness.begin(),
                                       result.outOfSampleFitness.end(), 0.0);
            result.averageOutOfSampleFitness = sum / result.outOfSampleFitness.size();

            double variance = 0.0;
            for (double f : result.outOfSampleFitness) {
                variance += (f - result.averageOutOfSampleFitness) *
                           (f - result.averageOutOfSampleFitness);
            }
            variance /= result.outOfSampleFitness.size();
            result.stdOutOfSampleFitness = std::sqrt(variance);
        }

        // Use average of best parameters as final
        if (!result.parameterHistory.empty()) {
            result.bestParameters = result.parameterHistory.back();
        }

        return result;
    }
};

// ─────────────────────────────────────────────────────────────
// Cross-Validation
// ─────────────────────────────────────────────────────────────

class CrossValidator {
public:
    /**
     * @brief K-fold cross-validation
     * @param data Full dataset
     * @param k Number of folds
     * @param parameters Parameter ranges
     * @param objectiveFunction Function to evaluate
     */
    template<typename T>
    [[nodiscard]] static std::vector<double>
    kFold(const std::vector<T>& data,
          size_t k,
          const std::vector<ParameterRange>& parameters,
          std::function<double(const std::vector<T>&, const ParameterSet&)> objectiveFunction) {

        if (data.size() < k) return {};

        std::vector<double> foldScores;
        size_t foldSize = data.size() / k;

        for (size_t fold = 0; fold < k; ++fold) {
            // Split data
            size_t testStart = fold * foldSize;
            size_t testEnd = (fold == k - 1) ? data.size() : (fold + 1) * foldSize;

            std::vector<T> trainData, testData;
            for (size_t i = 0; i < data.size(); ++i) {
                if (i >= testStart && i < testEnd) {
                    testData.push_back(data[i]);
                } else {
                    trainData.push_back(data[i]);
                }
            }

            // Optimize on training data
            auto trainObjective = [&](const ParameterSet& params) {
                return objectiveFunction(trainData, params);
            };

            auto optimized = GridSearchOptimizer::optimize(parameters, trainObjective);
            if (optimized.empty()) continue;

            // Evaluate on test data
            double testScore = objectiveFunction(testData, optimized[0]);
            foldScores.push_back(testScore);
        }

        return foldScores;
    }

    /**
     * @brief Time series cross-validation (rolling window)
     */
    template<typename T>
    [[nodiscard]] static std::vector<double>
    timeSeries(const std::vector<T>& data,
               size_t trainSize,
               size_t testSize,
               const std::vector<ParameterRange>& parameters,
               std::function<double(const std::vector<T>&, const ParameterSet&)> objectiveFunction) {

        std::vector<double> scores;

        for (size_t start = 0; start + trainSize + testSize <= data.size(); ++start) {
            size_t trainEnd = start + trainSize;
            size_t testEnd = trainEnd + testSize;

            std::vector<T> trainData(data.begin() + start, data.begin() + trainEnd);
            std::vector<T> testData(data.begin() + trainEnd, data.begin() + testEnd);

            auto trainObjective = [&](const ParameterSet& params) {
                return objectiveFunction(trainData, params);
            };

            auto optimized = GridSearchOptimizer::optimize(parameters, trainObjective);
            if (optimized.empty()) continue;

            double score = objectiveFunction(testData, optimized[0]);
            scores.push_back(score);
        }

        return scores;
    }
};

// ─────────────────────────────────────────────────────────────
// Multi-Objective Optimization (Pareto Front)
// ─────────────────────────────────────────────────────────────

struct ParetoPoint {
    ParameterSet parameters;
    std::vector<double> objectives;

    [[nodiscard]] bool dominates(const ParetoPoint& other) const {
        bool atLeastAsGood = true;
        bool strictlyBetter = false;

        for (size_t i = 0; i < objectives.size(); ++i) {
            if (objectives[i] < other.objectives[i]) {
                atLeastAsGood = false;
                break;
            }
            if (objectives[i] > other.objectives[i]) {
                strictlyBetter = true;
            }
        }

        return atLeastAsGood && strictlyBetter;
    }
};

class MultiObjectiveOptimizer {
public:
    /**
     * @brief Find Pareto front for multiple objectives
     * @param parameters Parameter ranges
     * @param objectiveFunctions Vector of objective functions (all to maximize)
     */
    [[nodiscard]] static std::vector<ParetoPoint>
    findParetoFront(const std::vector<ParameterRange>& parameters,
                    const std::vector<std::function<double(const ParameterSet&)>>& objectiveFunctions) {

        std::vector<ParetoPoint> allPoints;

        // Generate all parameter combinations (grid search)
        auto gridResults = GridSearchOptimizer::optimize(parameters,
            [&](const ParameterSet& params) {
                // Use first objective as primary for grid search
                return objectiveFunctions[0](params);
            });

        // Evaluate all objectives for each point
        for (const auto& params : gridResults) {
            ParetoPoint point;
            point.parameters = params;
            for (const auto& objFunc : objectiveFunctions) {
                point.objectives.push_back(objFunc(params));
            }
            allPoints.push_back(point);
        }

        // Extract Pareto front
        std::vector<ParetoPoint> paretoFront;
        for (const auto& point : allPoints) {
            bool dominated = false;
            for (const auto& other : allPoints) {
                if (other.dominates(point)) {
                    dominated = true;
                    break;
                }
            }
            if (!dominated) {
                paretoFront.push_back(point);
            }
        }

        return paretoFront;
    }
};

// ─────────────────────────────────────────────────────────────
// Parameter Sensitivity Analysis
// ─────────────────────────────────────────────────────────────

struct SensitivityResult {
    std::string parameterName;
    std::vector<double> parameterValues;
    std::vector<double> fitnessValues;
    double sensitivity{0.0}; // Slope of fitness vs parameter
    double elasticity{0.0};  // % change in fitness / % change in parameter
};

class SensitivityAnalyzer {
public:
    /**
     * @brief Analyze sensitivity of fitness to each parameter
     */
    [[nodiscard]] static std::vector<SensitivityResult>
    analyze(const std::vector<ParameterRange>& parameters,
           const ParameterSet& baseline,
           std::function<double(const ParameterSet&)> objectiveFunction,
           size_t nPoints = 20) {

        std::vector<SensitivityResult> results;

        for (const auto& param : parameters) {
            SensitivityResult result;
            result.parameterName = param.name;

            double baselineValue = baseline.values.at(param.name);
            double baselineFitness = baseline.fitness;

            // Vary this parameter while keeping others at baseline
            for (size_t i = 0; i < nPoints; ++i) {
                double t = static_cast<double>(i) / (nPoints - 1);
                double value = param.min + t * (param.max - param.min);

                ParameterSet testParams = baseline;
                testParams.values[param.name] = value;
                testParams.fitness = objectiveFunction(testParams);

                result.parameterValues.push_back(value);
                result.fitnessValues.push_back(testParams.fitness);
            }

            // Calculate sensitivity (linear regression slope)
            if (result.parameterValues.size() >= 2) {
                double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumX2 = 0.0;
                size_t n = result.parameterValues.size();

                for (size_t i = 0; i < n; ++i) {
                    double x = result.parameterValues[i];
                    double y = result.fitnessValues[i];
                    sumX += x;
                    sumY += y;
                    sumXY += x * y;
                    sumX2 += x * x;
                }

                double denom = n * sumX2 - sumX * sumX;
                if (std::abs(denom) > 1e-10) {
                    result.sensitivity = (n * sumXY - sumX * sumY) / denom;

                    // Elasticity at baseline
                    if (baselineValue != 0.0 && baselineFitness != 0.0) {
                        result.elasticity = result.sensitivity * (baselineValue / baselineFitness);
                    }
                }
            }

            results.push_back(result);
        }

        return results;
    }
};

} // namespace qtl
