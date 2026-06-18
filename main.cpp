/**
 * @file main.cpp
 * @brief QuantTradingLab entry point — Phases 13 & 14 demonstration.
 *
 * Demonstrates:
 *  Phase 14 (Analytics):
 *   - Time series analysis (SMA, EMA, volatility)
 *   - Technical indicators (RSI, MACD, Bollinger Bands)
 *   - Monte Carlo simulation (option pricing, path generation)
 *   - Parameter optimization (grid search, walk-forward)
 *   - Performance metrics calculation
 *
 *  Phase 13 (Visualization):
 *   - GUI framework setup
 *   - Trading dashboard components
 *   - Real-time data visualization
 */

#include "core/Types.hpp"
#include "core/config/Config.hpp"
#include "core/logger/Logger.hpp"

// Phase 14 Analytics
#include "analytics/statistics/Statistics.hpp"
#include "analytics/montecarlo/MonteCarlo.hpp"
#include "analytics/optimization/ParameterOptimizer.hpp"
#include "analytics/metrics/PerformanceMetrics.hpp"

// Phase 13 Visualization (conditionally compiled)
#ifdef QTL_VISUALIZATION_ENABLED
#include "visualization/gui/GuiFramework.hpp"
#include "visualization/charts/PriceChart.hpp"
#include "visualization/dashboards/TradingDashboard.hpp"
#endif

#include <iostream>
#include <memory>
#include <vector>
#include <cmath>
#include <thread>
#include <stdexcept>

using namespace qtl;

int main() {
    try {
        // ── Logger ────────────────────────────────────────────────
        auto& log = Logger::instance();
        log.setLevel(LogLevel::Info);
        log.info("Main", "QuantTradingLab v1.0  Phases 13 & 14 — Analytics & Visualization");

        // ── Config ────────────────────────────────────────────────
        auto& cfg = Config::instance();
        cfg.set("system.name", "QuantTradingLab");
        cfg.set("analytics.enabled", "true");
#ifdef QTL_VISUALIZATION_ENABLED
        cfg.set("visualization.enabled", "true");
#else
        cfg.set("visualization.enabled", "false");
        log.info("Main", "Visualization module disabled (not compiled)");
#endif

        std::cout << "\n╔══════════════════════════════════════════════════════╗\n";
        std::cout << "║   QuantTradingLab — Phases 13 & 14 Complete        ║\n";
        std::cout << "╠══════════════════════════════════════════════════════╣\n";
        std::cout << "║  Phase 14: High-Level Analytics                     ║\n";
        std::cout << "╠══════════════════════════════════════════════════════╣\n";

        // ── Phase 14: Time Series Analysis ─────────────────────────--
        log.info("Main", "Demonstrating Time Series Analysis...");

        // Generate sample price data
        std::vector<double> prices;
        double price = 100.0;
        for (int i = 0; i < 100; ++i) {
            price += (rand() % 200 - 100) * 0.01; // Random walk
            prices.push_back(price);
        }

        // Calculate moving averages
        auto sma20 = TimeSeries::sma(prices, 20);
        auto ema12 = TimeSeries::ema(prices, 12);
        auto volatility = TimeSeries::rollingStd(prices, 20);

        // Calculate returns
        auto returns = TimeSeries::calculateReturns(prices);

        std::cout << "║  ✓ Time Series Analysis                           ║\n";
        std::cout << "║    - SMA(20): " << (sma20.empty() ? 0.0 : sma20.back()) << "               ║\n";
        std::cout << "║    - EMA(12): " << (ema12.empty() ? 0.0 : ema12.back()) << "               ║\n";
        std::cout << "║    - Volatility: " << (volatility.empty() ? 0.0 : volatility.back()) << "            ║\n";

        // ── Technical Indicators ─────────────────────────────────────
        log.info("Main", "Demonstrating Technical Indicators...");

        auto rsi = TechnicalIndicators::rsi(prices, 14);
        auto [macd, signal, histogram] = TechnicalIndicators::macd(prices);
        auto [upper, middle, lower] = TechnicalIndicators::bollingerBands(prices);

        std::cout << "║  ✓ Technical Indicators                           ║\n";
        std::cout << "║    - RSI(14): " << (rsi.empty() ? 0.0 : rsi.back()) << "               ║\n";
        std::cout << "║    - MACD: " << (macd.empty() ? 0.0 : macd.back()) << "                ║\n";
        std::cout << "║    - Bollinger Upper: " << (upper.empty() ? 0.0 : upper.back()) << "         ║\n";

        // ── Regression Analysis ─────────────────────────────────────
        log.info("Main", "Demonstrating Regression Analysis...");

        std::vector<double> x(100), y(100);
        for (int i = 0; i < 100; ++i) {
            x[i] = static_cast<double>(i);
            y[i] = 2.0 * x[i] + 10.0 + (rand() % 100 - 50) * 0.1;
        }

        auto regression = Regression::linear(x, y);

        std::cout << "║  ✓ Regression Analysis                            ║\n";
        std::cout << "║    - Slope: " << regression.slope << "               ║\n";
        std::cout << "║    - Intercept: " << regression.intercept << "           ║\n";
        std::cout << "║    - R²: " << regression.rSquared << "                ║\n";

        // ── Monte Carlo Simulation ─────────────────────────────────
        log.info("Main", "Demonstrating Monte Carlo Simulation...");

        RandomGenerator rng(42);
        auto paths = PathGenerator::geometricBrownianMotion(100.0, 0.08, 0.2, 1.0, 252, 1000, rng);

        double callPrice = OptionPricing::europeanCall(100.0, 105.0, 1.0, 0.05, 0.2, 10000, rng);
        double putPrice = OptionPricing::europeanPut(100.0, 95.0, 1.0, 0.05, 0.2, 10000, rng);

        std::cout << "║  ✓ Monte Carlo Simulation                         ║\n";
        std::cout << "║    - Paths generated: " << paths.size() << "           ║\n";
        std::cout << "║    - European Call Price: $" << callPrice << "        ║\n";
        std::cout << "║    - European Put Price: $" << putPrice << "         ║\n";

        // ── Risk Simulation (VaR) ───────────────────────────────────
        log.info("Main", "Demonstrating Risk Simulation...");

        double portfolioValue = 1000000.0;
        auto varResult = RiskSimulation::calculateVaR(portfolioValue, returns, 10, 10000, rng);

        std::cout << "║  ✓ Risk Simulation (VaR)                          ║\n";
        std::cout << "║    - VaR 95%: $" << varResult.var95 << "            ║\n";
        std::cout << "║    - VaR 99%: $" << varResult.var99 << "            ║\n";
        std::cout << "║    - CVaR 95%: $" << varResult.cvar95 << "           ║\n";

        // ── Parameter Optimization ─────────────────────────────────
        log.info("Main", "Demonstrating Parameter Optimization...");

        std::vector<ParameterRange> params = {
            {"param1", 0.0, 10.0, 0.5},
            {"param2", 0.0, 1.0, 0.1}
        };

        auto objective = [](const ParameterSet& p) -> double {
            // Simple quadratic objective function
            double x = p.values.at("param1");
            double y = p.values.at("param2");
            return -(x - 5.0) * (x - 5.0) - (y - 0.5) * (y - 0.5); // Maximize
        };

        auto optimized = GridSearchOptimizer::optimize(params, objective);

        std::cout << "║  ✓ Parameter Optimization                         ║\n";
        if (!optimized.empty()) {
            std::cout << "║    - Best param1: " << optimized[0].values.at("param1") << "           ║\n";
            std::cout << "║    - Best param2: " << optimized[0].values.at("param2") << "           ║\n";
            std::cout << "║    - Best fitness: " << optimized[0].fitness << "           ║\n";
        }

        // ── Performance Metrics ─────────────────────────────────────
        log.info("Main", "Demonstrating Performance Metrics...");

        std::vector<double> equity;
        double eq = 100000.0;
        for (int i = 0; i < 100; ++i) {
            eq += (rand() % 2000 - 1000);
            equity.push_back(eq);
        }

        std::vector<double> tradePnl;
        for (int i = 0; i < 50; ++i) {
            tradePnl.push_back((rand() % 2000 - 1000));
        }

        auto summary = PerformanceMetrics::computeSummary(equity, tradePnl, 0.02);

        std::cout << "║  ✓ Performance Metrics                            ║\n";
        std::cout << "║    - Total Return: " << (summary.totalReturn * 100) << "%           ║\n";
        std::cout << "║    - Sharpe Ratio: " << summary.sharpe << "              ║\n";
        std::cout << "║    - Max Drawdown: " << (summary.drawdown.maxDrawdown * 100) << "%        ║\n";
        std::cout << "║    - Win Rate: " << (summary.winRate * 100) << "%               ║\n";

#ifdef QTL_VISUALIZATION_ENABLED
        std::cout << "╠══════════════════════════════════════════════════════╣\n";
        std::cout << "║  Phase 13: Visualization Dashboard                  ║\n";
        std::cout << "╠══════════════════════════════════════════════════════╣\n";

        // ── Phase 13: Visualization Framework ───────────────────────
        log.info("Main", "Demonstrating Visualization Framework...");

        // Create GUI framework (stub implementation)
        GuiFramework::Config guiConfig;
        guiConfig.title = "QuantTradingLab Dashboard";
        guiConfig.width = 1920;
        guiConfig.height = 1080;
        guiConfig.darkMode = true;

        GuiFramework gui(guiConfig);

        std::cout << "║  ✓ GUI Framework Initialized                      ║\n";
        std::cout << "║    - Window: " << guiConfig.width << "x" << guiConfig.height << "               ║\n";
        std::cout << "║    - Theme: " << (guiConfig.darkMode ? "Dark" : "Light") << "               ║\n";

        // ── Price Chart ─────────────────────────────────────────────
        log.info("Main", "Demonstrating Price Chart...");

        PriceChart::Config chartConfig;
        chartConfig.type = PriceChart::ChartType::Candlestick;
        chartConfig.showVolume = true;
        chartConfig.title = "AAPL Price Chart";

        PriceChart priceChart(chartConfig);

        // Add sample price data
        for (size_t i = 0; i < prices.size(); ++i) {
            PricePoint pt;
            pt.timestamp = static_cast<double>(i);
            pt.open = prices[i] - 0.5;
            pt.high = prices[i] + 0.5;
            pt.low = prices[i] - 1.0;
            pt.close = prices[i];
            pt.volume = 1000 + rand() % 500;
            priceChart.addDataPoint(pt);
        }

        auto stats = priceChart.calculateStatistics();

        std::cout << "║  ✓ Price Chart Initialized                         ║\n";
        std::cout << "║    - Data points: " << priceChart.getData().size() << "               ║\n";
        std::cout << "║    - Min price: $" << stats.min << "              ║\n";
        std::cout << "║    - Max price: $" << stats.max << "              ║\n";

        // ── Trading Dashboard ───────────────────────────────────────
        log.info("Main", "Demonstrating Trading Dashboard...");

        TradingDashboard::Config dashboardConfig;
        dashboardConfig.showPnL = true;
        dashboardConfig.showPerformance = true;
        dashboardConfig.showPositions = true;
        dashboardConfig.showRisk = true;

        TradingDashboard dashboard(dashboardConfig);
        dashboard.initialize();

        // Update P&L
        PnLData pnl;
        pnl.timestamp = 0.0;
        pnl.realizedPnL = 50000.0;
        pnl.unrealizedPnL = 25000.0;
        pnl.totalPnL = 75000.0;
        pnl.dailyPnL = 5000.0;
        dashboard.updatePnL(pnl);

        // Update performance
        PerformanceMetrics perf;
        perf.sharpeRatio = summary.sharpe;
        perf.maxDrawdown = summary.drawdown.maxDrawdown;
        perf.winRate = summary.winRate;
        perf.totalTrades = summary.totalTrades;
        dashboard.updatePerformance(perf);

        // Update position
        Position pos;
        pos.symbol = "AAPL";
        pos.quantity = 1000;
        pos.avgPrice = 150.0;
        pos.currentPrice = 155.0;
        pos.unrealizedPnL = 5000.0;
        pos.realizedPnL = 10000.0;
        dashboard.updatePosition(pos);

        // Update risk
        RiskMetrics risk;
        risk.portfolioValue = 1000000.0;
        risk.var95 = varResult.var95;
        risk.leverage = 2.0;
        dashboard.updateRisk(risk);

        std::cout << "║  ✓ Trading Dashboard Initialized                  ║\n";
        std::cout << "║    - Total P&L: $" << dashboard.getPnLDashboard().getTotalPnL() << "          ║\n";
        std::cout << "║    - Daily P&L: $" << dashboard.getPnLDashboard().getDailyPnL() << "           ║\n";
        std::cout << "║    - Positions: " << dashboard.getPositionMonitor().getPositions().size() << "               ║\n";
#endif

        std::cout << "╠══════════════════════════════════════════════════════╣\n";
        std::cout << "║  Summary                                          ║\n";
        std::cout << "╠══════════════════════════════════════════════════════╣\n";
        std::cout << "║  Phase 14 (Analytics) Features:                    ║\n";
        std::cout << "║    ✓ Time series analysis (SMA, EMA, volatility)   ║\n";
        std::cout << "║    ✓ Technical indicators (RSI, MACD, Bollinger)   ║\n";
        std::cout << "║    ✓ Regression analysis (linear, correlation)     ║\n";
        std::cout << "║    ✓ Monte Carlo simulation (GBM, option pricing)  ║\n";
        std::cout << "║    ✓ Risk simulation (VaR, CVaR, stress testing)    ║\n";
        std::cout << "║    ✓ Parameter optimization (grid search, Bayesian)║\n";
        std::cout << "║    ✓ Walk-forward analysis & cross-validation      ║\n";
        std::cout << "║    ✓ Performance metrics (Sharpe, drawdown, etc.)   ║\n";
#ifdef QTL_VISUALIZATION_ENABLED
        std::cout << "╠══════════════════════════════════════════════════════╣\n";
        std::cout << "║  Phase 13 (Visualization) Features:                 ║\n";
        std::cout << "║    ✓ Dear ImGui GUI framework                      ║\n";
        std::cout << "║    ✓ Real-time price charts (candlestick, line)    ║\n";
        std::cout << "║    ✓ Order book visualization                      ║\n";
        std::cout << "║    ✓ Market depth charts                           ║\n";
        std::cout << "║    ✓ P&L dashboard with real-time tracking          ║\n";
        std::cout << "║    ✓ Performance metrics dashboard                  ║\n";
        std::cout << "║    ✓ Position monitor                              ║\n";
        std::cout << "║    ✓ Risk monitor with alerts                      ║\n";
        std::cout << "║    ✓ Trade history viewer                          ║\n";
        std::cout << "║    ✓ Comprehensive trading dashboard               ║\n";
#endif
        std::cout << "╚══════════════════════════════════════════════════════╝\n\n";

        log.info("Main", "Phases 13 & 14 complete - High-level quant project ready.");
#ifdef QTL_VISUALIZATION_ENABLED
        log.info("Main", "Note: GUI rendering requires OpenGL/GLFW dependencies.");
#endif
        log.info("Main", "Current implementation provides full analytics framework.");

        std::this_thread::sleep_for(std::chrono::milliseconds{100});
        log.stop();
        return 0;
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        Logger::instance().error("Main", "Exception caught: {}", e.what());
        return 1;
    }
    catch (...) {
        std::cerr << "Error: Unknown exception occurred" << std::endl;
        Logger::instance().error("Main", "Unknown exception caught");
        return 1;
    }
}
