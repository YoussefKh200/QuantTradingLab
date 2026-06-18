/**
 * @file tests/test_main.cpp
 * @brief Minimal hand-rolled test runner.
 */

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

static std::vector<TestCase> g_tests;

void registerTest(std::string name, std::function<void()> fn) {
    g_tests.push_back({std::move(name), std::move(fn)});
}

// Forward declarations
void registerEventSystemTests();
void registerOrderBookTests();
void registerMatchingEngineTests();
void registerRiskEngineTests();
void registerPortfolioTests();
void registerLowLatencyTests();
void registerPerformanceMetricsTests();
void registerMarketDataTests();
void registerStrategyFrameworkTests();
void registerBlackScholesTests();
void registerDealerPositioningTests();
void registerPortfolioTests();
void registerLowLatencyTests();
void registerBlackScholesTests();
void registerDealerPositioningTests();
void registerPortfolioTests();
void registerLowLatencyTests();
void registerBlackScholesTests();
void registerDealerPositioningTests();
void registerPortfolioTests();
void registerLowLatencyTests();
void registerBlackScholesTests();
void registerDealerPositioningTests();
void registerPortfolioTests();
void registerLowLatencyTests();
void registerBlackScholesTests();
void registerDealerPositioningTests();
void registerPortfolioTests();
void registerLowLatencyTests();
void registerBlackScholesTests();
void registerDealerPositioningTests();
void registerPortfolioTests();
void registerLowLatencyTests();
void registerBlackScholesTests();
void registerDealerPositioningTests();
void registerPortfolioTests();
void registerLowLatencyTests();

int main() {
    registerEventSystemTests();
    registerOrderBookTests();
    registerMatchingEngineTests();
    registerRiskEngineTests();
    registerPortfolioTests();
    registerLowLatencyTests();
    registerPerformanceMetricsTests();
    registerMarketDataTests();
    registerStrategyFrameworkTests();
    registerBlackScholesTests();
    registerDealerPositioningTests();
    registerPortfolioTests();
    registerLowLatencyTests();
    registerBlackScholesTests();
    registerDealerPositioningTests();
    registerPortfolioTests();
    registerLowLatencyTests();
    registerBlackScholesTests();
    registerDealerPositioningTests();
    registerPortfolioTests();
    registerLowLatencyTests();
    registerBlackScholesTests();
    registerDealerPositioningTests();
    registerPortfolioTests();
    registerLowLatencyTests();
    registerBlackScholesTests();
    registerDealerPositioningTests();
    registerPortfolioTests();
    registerLowLatencyTests();
    registerBlackScholesTests();
    registerDealerPositioningTests();
    registerPortfolioTests();
    registerLowLatencyTests();
    registerBlackScholesTests();
    registerDealerPositioningTests();
    registerPortfolioTests();
    registerLowLatencyTests();

    int passed = 0, failed = 0;
    std::vector<std::string> failures;

    for (auto& tc : g_tests) {
        std::cout << "[RUN ] " << tc.name << '\n'; std::cout.flush();
        try {
            tc.fn();
            std::cout << "[PASS] " << tc.name << '\n'; std::cout.flush();
            ++passed;
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << tc.name << " — " << e.what() << '\n'; std::cout.flush();
            ++failed;
            failures.push_back(tc.name);
        } catch (...) {
            std::cout << "[FAIL] " << tc.name << " — unknown exception\n";
            ++failed;
            failures.push_back(tc.name);
        }
    }

    std::cout << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n";
    std::cout << "Results: " << passed << " passed, "
              << failed << " failed / " << g_tests.size() << " total\n";

    if (failed > 0) {
        std::cout << "Failed tests:\n";
        for (auto& f : failures) std::cout << "  • " << f << '\n';
        return 1;
    }
    return 0;
}
