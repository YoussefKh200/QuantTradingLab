/**
 * @file tests/test_main.cpp
 * @brief Minimal hand-rolled test runner.
 *
 * Each test_*.cpp provides a void run_<module>_tests() function.
 * This file calls them all and reports pass/fail.
 * GoogleTest will be added as an optional dependency in a later phase.
 */

#include <iostream>
#include <string>
#include <vector>
#include <functional>
#include <stdexcept>

// ── Test registry ─────────────────────────────────────────────

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

static std::vector<TestCase> g_tests;

void registerTest(std::string name, std::function<void()> fn) {
    g_tests.push_back({std::move(name), std::move(fn)});
}

// Forward declarations from each test translation unit
void registerEventSystemTests();
void registerOrderBookTests();
void registerMatchingEngineTests();
void registerRiskEngineTests();
void registerPortfolioTests();
void registerBlackScholesTests();
void registerPerformanceMetricsTests();

int main() {
    // Register all test suites
    registerEventSystemTests();
    registerOrderBookTests();
    registerMatchingEngineTests();
    registerRiskEngineTests();
    registerPortfolioTests();
    registerBlackScholesTests();
    registerPerformanceMetricsTests();

    int passed = 0, failed = 0;
    std::vector<std::string> failures;

    for (auto& tc : g_tests) {
        try {
            tc.fn();
            std::cout << "[PASS] " << tc.name << '\n';
            ++passed;
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << tc.name << " — " << e.what() << '\n';
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
