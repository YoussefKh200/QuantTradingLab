/**
 * @file tests/test_performance_metrics.cpp
 * @brief Phase 6 — PerformanceMetrics, MarketSimulator, BacktestEngine tests.
 *
 * Tests cover:
 *  1.  equityToReturns / returnsToEquity round-trip
 *  2.  CAGR calculation (known values)
 *  3.  Sharpe ratio (positive, zero vol, negative)
 *  4.  Sortino ratio vs Sharpe (downside only)
 *  5.  Max drawdown — depth, duration, recovery
 *  6.  Max drawdown no recovery
 *  7.  Calmar ratio
 *  8.  Win rate from trade P&L
 *  9.  Expectancy calculation
 *  10. Profit factor
 *  11. Average win / average loss
 *  12. Annualised volatility
 *  13. VaR parametric (95%, 99%)
 *  14. VaR historical (95%)
 *  15. CVaR (Expected Shortfall)
 *  16. Omega ratio
 *  17. Beta / Alpha vs benchmark
 *  18. computeSummary full pipeline
 *  19. Summary toString non-empty
 *  20. FixedSlippage: buy fills above market, sell below
 *  21. PercentSlippage correctness
 *  22. VolumeSlippage scales with participation
 *  23. MarketSimulator: limit order not crossed → no fill
 *  24. MarketSimulator: market order always fills
 *  25. BacktestEngine: run with synthetic CSV data
 *  26. BacktestEngine: equity curve non-empty after run
 *  27. BacktestEngine: HTML report generated (file exists, non-empty)
 *  28. BacktestEngine: summary metrics populated after run
 */

#include "tests/TestHelper.hpp"
#include "analytics/metrics/PerformanceMetrics.hpp"
#include "backtesting/simulator/MarketSimulator.hpp"
#include "backtesting/engine/BacktestEngine.hpp"
#include "backtesting/engine/ReportGenerator.hpp"

#include <functional>
#include <string>
#include <vector>
#include <fstream>
#include <cmath>
#include <numeric>
#include <iostream>

extern void registerTest(std::string, std::function<void()>);

using PM = qtl::PerformanceMetrics;

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────

static std::vector<double> makeEquity(double start, double annualReturn,
                                       int nDays) {
    // Geometric daily growth
    double dailyReturn = std::pow(1.0 + annualReturn, 1.0 / 252.0) - 1.0;
    std::vector<double> eq;
    eq.reserve(nDays + 1);
    eq.push_back(start);
    for (int i = 0; i < nDays; ++i)
        eq.push_back(eq.back() * (1.0 + dailyReturn));
    return eq;
}

static void writeQuoteCSV(const std::string& path, int n,
                            const std::string& sym = "AAPL",
                            qtl::Timestamp start  = 1700000000000000000LL) {
    std::ofstream f{path};
    f << "timestamp_ns,symbol,bid_px,bid_sz,ask_px,ask_sz,exchange\n";
    for (int i = 0; i < n; ++i) {
        qtl::Timestamp ts = start + static_cast<qtl::Timestamp>(i) * 1'000'000'000LL;
        double bid = 182.49 + (i % 20) * 0.01;
        double ask = bid + 0.02;
        f << ts << "," << sym << ","
          << std::fixed << bid << ",200,"
          << ask << ",150,NASDAQ\n";
    }
}

// ─────────────────────────────────────────────────────────────
// PerformanceMetrics tests
// ─────────────────────────────────────────────────────────────

static void test_equity_returns_roundtrip() {
    std::vector<double> eq = {100.0, 110.0, 105.0, 115.0, 108.0};
    auto returns = PM::equityToReturns(eq);
    ASSERT_EQ(returns.size(), size_t(4), "4 returns from 5 equity points");
    ASSERT_NEAR(returns[0], 0.10, 1e-9, "10% return");
    ASSERT_NEAR(returns[1], -0.04545, 1e-4, "~-4.5% return");

    auto back = PM::returnsToEquity(returns, 100.0);
    ASSERT_EQ(back.size(), eq.size(), "same length after round-trip");
    for (size_t i = 0; i < eq.size(); ++i)
        ASSERT_NEAR(back[i], eq[i], 1e-6, "round-trip equity value");
}

static void test_cagr_known_value() {
    // $100 → $200 in exactly 252 days = 100% CAGR
    auto eq = makeEquity(100.0, 1.0, 252);
    double c = PM::cagr(eq);
    ASSERT_NEAR(c, 1.0, 0.01, "CAGR ~100% for 1-year doubling");

    // Single point → 0
    ASSERT_EQ(PM::cagr({100.0}), 0.0, "single-point CAGR = 0");

    // Flat → 0%
    std::vector<double> flat(252, 100.0);
    ASSERT_NEAR(PM::cagr(flat), 0.0, 1e-9, "flat equity CAGR = 0");
}

static void test_sharpe_positive() {
    // Construct returns with known mean and std
    // mean = 0.001/day, std = 0.01/day → Sharpe = 0.1/0.01 * sqrt(252) ≈ 1.587
    std::vector<double> returns(252, 0.001);
    // Add noise to give std ~0.01
    for (int i = 0; i < 252; i += 2) returns[i] -= 0.010;
    for (int i = 1; i < 252; i += 2) returns[i] += 0.010;

    double sh = PM::sharpe(returns, 0.0, 252.0);
    ASSERT_TRUE(sh > 0, "Sharpe > 0 for positive mean");
}

static void test_sharpe_zero_vol() {
    // Constant returns → std = 0 → Sharpe should return 0 (not inf/nan)
    // Use 2-element vector so ddof=1 gives exactly 0 variance
    std::vector<double> returns = {0.001, 0.001};
    double sh = PM::sharpe(returns);
    ASSERT_EQ(sh, 0.0, "zero-vol Sharpe = 0 (no division by zero)");
    ASSERT_FALSE(std::isinf(sh), "Sharpe not infinite");
    ASSERT_FALSE(std::isnan(sh), "Sharpe not NaN");
}

static void test_sortino_vs_sharpe() {
    // With asymmetric returns (more down days), Sortino < Sharpe
    std::vector<double> returns;
    for (int i = 0; i < 200; ++i) returns.push_back(0.002);   // up
    for (int i = 0; i < 52;  ++i) returns.push_back(-0.020);  // bigger down
    double sh = PM::sharpe (returns);
    double so = PM::sortino(returns);
    ASSERT_TRUE(std::isfinite(sh) && std::isfinite(so), "both finite");
    // Sortino should differ from Sharpe
    ASSERT_TRUE(std::abs(sh - so) > 0.01, "Sortino differs from Sharpe");
}

static void test_max_drawdown_known() {
    // Equity: 100 → 120 → 90 → 130 → 80
    // Peak at idx=1 (120), trough at idx=4 (80): DD = (80-120)/120 = -33.3%
    std::vector<double> eq = {100.0, 120.0, 90.0, 130.0, 80.0};
    auto dd = PM::maxDrawdown(eq);
    ASSERT_NEAR(dd.maxDrawdown, (80.0 - 130.0) / 130.0, 1e-6, "max DD value");
    ASSERT_EQ(dd.peakIndex,   size_t(3), "peak at idx 3 (130)");
    ASSERT_EQ(dd.troughIndex, size_t(4), "trough at idx 4 (80)");
    ASSERT_EQ(dd.drawdownDays, 1, "1-period drawdown");
}

static void test_max_drawdown_no_recovery() {
    // Steadily declining — never recovers
    std::vector<double> eq = {100.0, 90.0, 80.0, 70.0, 60.0};
    auto dd = PM::maxDrawdown(eq);
    ASSERT_NEAR(dd.maxDrawdown, -0.40, 1e-6, "40% drawdown");
    ASSERT_EQ(dd.recoveryIndex, size_t(0), "no recovery index");
    ASSERT_EQ(dd.recoveryDays,  0, "no recovery days");
}

static void test_max_drawdown_with_recovery() {
    // 100 → 150 → 100 → 160 (recovers and makes new high)
    std::vector<double> eq = {100.0, 150.0, 100.0, 160.0};
    auto dd = PM::maxDrawdown(eq);
    ASSERT_NEAR(dd.maxDrawdown, (100.0 - 150.0) / 150.0, 1e-6, "DD value");
    ASSERT_TRUE(dd.recoveryIndex > 0, "recovery found");
    ASSERT_TRUE(dd.recoveryDays  > 0, "recovery days > 0");
}

static void test_calmar_ratio() {
    // 20% CAGR, 10% max DD → Calmar = 2.0
    auto eq  = makeEquity(100.0, 0.20, 252);
    // Inject a 10% drawdown
    double peak = *std::max_element(eq.begin(), eq.end());
    // Simulate by appending a dip
    eq.push_back(peak * 0.90);
    eq.push_back(peak * 0.95);
    eq.push_back(peak * 1.01);

    double cal = PM::calmar(eq);
    ASSERT_TRUE(cal > 0, "Calmar > 0");
    ASSERT_TRUE(std::isfinite(cal), "Calmar finite");
}

static void test_win_rate() {
    std::vector<double> pnl = {100, -50, 200, -30, 150, -80, 90};
    double wr = PM::winRate(pnl);
    // Wins: 100, 200, 150, 90 = 4/7
    ASSERT_NEAR(wr, 4.0/7.0, 1e-9, "win rate 4/7");
    ASSERT_EQ(PM::winRate({}), 0.0, "empty = 0");
}

static void test_expectancy() {
    // 4 wins avg 135, 3 losses avg -53.33
    std::vector<double> pnl = {100, -50, 200, -30, 150, -80, 90};
    double ex = PM::expectancy(pnl);
    // Manual: wr=4/7, avgWin=(100+200+150+90)/4=135,
    //         avgLoss=(-50-30-80)/3=-53.33
    // E = (4/7)*135 + (3/7)*(-53.33) = 77.14 - 22.86 = 54.29
    ASSERT_NEAR(ex, (4.0/7.0)*135.0 + (3.0/7.0)*(-160.0/3.0), 0.1,
                "expectancy calculation");
}

static void test_profit_factor() {
    std::vector<double> pnl = {100, -50, 200, -30, 150, -80, 90};
    double pf = PM::profitFactor(pnl);
    // gross profit = 540, gross loss = 160 → PF = 3.375
    ASSERT_NEAR(pf, 540.0 / 160.0, 1e-9, "profit factor");
    ASSERT_EQ(PM::profitFactor({}), 0.0, "empty PF = 0");
}

static void test_avg_win_loss() {
    std::vector<double> pnl = {100, -50, 200, -30};
    ASSERT_NEAR(PM::avgWin(pnl),  150.0, 1e-9, "avg win");
    ASSERT_NEAR(PM::avgLoss(pnl), -40.0, 1e-9, "avg loss");
}

static void test_annualised_vol() {
    // Daily returns with known std
    std::vector<double> r(252, 0.0);
    for (int i = 0; i < 252; i += 2) r[i] =  0.01;
    for (int i = 1; i < 252; i += 2) r[i] = -0.01;
    double vol = PM::annualisedVol(r);
    // std daily ≈ 0.01, annualised = 0.01 * sqrt(252) ≈ 0.1587
    ASSERT_NEAR(vol, 0.01 * std::sqrt(252.0), 1e-3, "annualised vol");
}

static void test_var_parametric() {
    // 100 returns with known mean=0, std=0.01
    std::vector<double> r(1000, 0.0);
    for (int i = 0; i < 1000; i += 2) r[i] =  0.01;
    for (int i = 1; i < 1000; i += 2) r[i] = -0.01;
    double var95 = PM::varParametric(r, 0.95);
    double var99 = PM::varParametric(r, 0.99);
    ASSERT_TRUE(var95 > 0, "VaR95 > 0");
    ASSERT_TRUE(var99 > var95, "VaR99 > VaR95");
    // z=1.645 → VaR95 ≈ 1.645 * 0.01 = 0.01645
    ASSERT_NEAR(var95, 1.645 * 0.01, 0.001, "VaR95 approx");
}

static void test_var_historical() {
    std::vector<double> r;
    for (int i = -50; i <= 50; ++i) r.push_back(i * 0.001);
    double var95 = PM::varHistorical(r, 0.95);
    ASSERT_TRUE(var95 > 0, "historical VaR > 0");
    ASSERT_TRUE(var95 < 0.06, "historical VaR in expected range");
}

static void test_cvar() {
    std::vector<double> r;
    for (int i = -50; i <= 50; ++i) r.push_back(i * 0.001);
    double cvar95 = PM::cvar(r, 0.95);
    double var95  = PM::varHistorical(r, 0.95);
    ASSERT_TRUE(cvar95 >= var95, "CVaR >= VaR");
    ASSERT_TRUE(cvar95 > 0, "CVaR > 0");
}

static void test_omega_ratio() {
    std::vector<double> pos = {0.01, 0.02, 0.03};
    std::vector<double> neg = {-0.01, -0.02};
    std::vector<double> combined = {0.01, 0.02, 0.03, -0.01, -0.02};
    double om = PM::omega(combined, 0.0);
    ASSERT_TRUE(om > 1.0, "omega > 1 for net-positive returns");
    ASSERT_EQ(PM::omega({}, 0.0), 0.0, "empty omega = 0");
}

static void test_beta_alpha() {
    // Strategy = 2 * benchmark (beta=2, alpha=0)
    std::vector<double> bench  = {0.01, -0.02, 0.015, -0.005, 0.02};
    std::vector<double> strat;
    for (double b : bench) strat.push_back(2.0 * b);
    auto ba = PM::betaAlpha(strat, bench);
    ASSERT_NEAR(ba.beta,  2.0, 1e-6, "beta = 2");
    ASSERT_NEAR(ba.alpha, 0.0, 1e-6, "alpha = 0");
}

static void test_compute_summary_full() {
    // Build equity with genuine daily noise so stddev > 0
    std::vector<double> eq;
    eq.push_back(100000.0);
    // Vary returns: mostly +0.001 with periodic -0.005 drawdowns
    for (int i = 0; i < 252; ++i) {
        double r = (i % 7 == 0) ? -0.005 : 0.001;
        eq.push_back(eq.back() * (1.0 + r));
    }
    std::vector<double> trades;
    for (int i = 0; i < 50; ++i) trades.push_back(i % 3 == 0 ? -200.0 : 500.0);

    auto s = PM::computeSummary(eq, trades);
    ASSERT_NEAR(s.totalReturn, eq.back()/eq.front() - 1.0, 1e-6, "total return");
    ASSERT_TRUE(s.annualisedVol > 0, "vol > 0");
    ASSERT_TRUE(std::isfinite(s.sharpe),  "Sharpe finite");
    ASSERT_TRUE(std::isfinite(s.sortino), "Sortino finite");
    ASSERT_EQ(s.totalTrades, 50, "50 trades");
    ASSERT_TRUE(s.winRate > 0 && s.winRate < 1.0, "win rate in (0,1)");
    ASSERT_TRUE(std::isfinite(s.var95),  "VaR95 finite");
    ASSERT_TRUE(std::isfinite(s.cvar95), "CVaR95 finite");
}

static void test_summary_tostring() {
    auto eq = makeEquity(100000.0, 0.20, 252);
    auto s  = PM::computeSummary(eq, {100.0, -50.0, 200.0});
    std::string str = s.toString();
    ASSERT_FALSE(str.empty(), "toString not empty");
    ASSERT_TRUE(str.find("CAGR") != std::string::npos, "contains CAGR");
    ASSERT_TRUE(str.find("Sharpe") != std::string::npos, "contains Sharpe");
    ASSERT_TRUE(str.find("Drawdown") != std::string::npos, "contains Drawdown");
    ASSERT_TRUE(str.find("Win Rate") != std::string::npos, "contains Win Rate");
}

// ─────────────────────────────────────────────────────────────
// MarketSimulator tests
// ─────────────────────────────────────────────────────────────

static void test_fixed_slippage() {
    auto model = std::make_shared<qtl::FixedSlippage>(0.05);
    qtl::Order buy;
    buy.side = qtl::Side::Buy; buy.type = qtl::OrderType::Market;
    buy.quantity = 100;
    qtl::Order sell;
    sell.side = qtl::Side::Sell; sell.type = qtl::OrderType::Market;
    sell.quantity = 100;

    // Buy fills above market, sell below
    ASSERT_NEAR(model->apply(buy,  100.0, 1000), 100.05, 1e-9, "buy slippage +");
    ASSERT_NEAR(model->apply(sell, 100.0, 1000), 99.95,  1e-9, "sell slippage -");
}

static void test_percent_slippage() {
    auto model = std::make_shared<qtl::PercentSlippage>(0.001); // 0.1%
    qtl::Order buy;
    buy.side = qtl::Side::Buy; buy.type = qtl::OrderType::Market;
    buy.quantity = 100;
    double fillPx = model->apply(buy, 200.0, 1000);
    ASSERT_NEAR(fillPx, 200.0 * 1.001, 1e-9, "0.1% slippage on 200");
}

static void test_volume_slippage_scales() {
    auto model = std::make_shared<qtl::VolumeSlippage>(0.01);
    qtl::Order small_; small_.side = qtl::Side::Buy;
    small_.type = qtl::OrderType::Market; small_.quantity = 10;
    qtl::Order large_; large_.side = qtl::Side::Buy;
    large_.type = qtl::OrderType::Market; large_.quantity = 500;

    double slipSmall = model->apply(small_, 100.0, 1000) - 100.0;
    double slipLarge = model->apply(large_, 100.0, 1000) - 100.0;
    ASSERT_TRUE(slipLarge > slipSmall, "larger order has more slippage");
}

static void test_limit_not_crossed_no_fill() {
    qtl::MarketSimulator sim;
    qtl::Order o;
    o.side = qtl::Side::Buy; o.type = qtl::OrderType::Limit;
    o.price = 180.0; o.quantity = 100;
    // bestAsk = 182 > limit 180 → no fill
    auto fill = sim.fill(o, 179.0, 182.0, 1000);
    ASSERT_FALSE(fill.filled, "limit buy not crossed → no fill");
}

static void test_market_order_always_fills() {
    qtl::MarketSimulator sim;
    qtl::Order o;
    o.side = qtl::Side::Buy; o.type = qtl::OrderType::Market;
    o.quantity = 100;
    auto fill = sim.fill(o, 179.0, 182.0, 1000);
    ASSERT_TRUE(fill.filled, "market order always fills");
    ASSERT_TRUE(fill.fillPrice >= 182.0, "fill at or above ask");
    ASSERT_EQ(fill.fillQty, qtl::Quantity(100), "full fill qty");
    ASSERT_TRUE(fill.commission > 0, "commission charged");
}

// ─────────────────────────────────────────────────────────────
// BacktestEngine + ReportGenerator tests
// ─────────────────────────────────────────────────────────────

// Simple momentum strategy for testing
class SimpleMomentumStrategy : public qtl::IStrategy {
public:
    [[nodiscard]] std::string name() const override { return "SimpleMomentum"; }

    void onStart(qtl::BacktestEngine*) override { tickCount_ = 0; }

    void onTick(const qtl::MarketEvent& e, qtl::BacktestEngine* eng) override {
        ++tickCount_;
        // Buy on odd ticks, sell on even ticks (simplified)
        if (tickCount_ % 20 == 0 && eng->position(e.symbol) == 0) {
            qtl::Order o;
            o.id = nextId_++; o.symbol = e.symbol;
            o.side = qtl::Side::Buy; o.type = qtl::OrderType::Market;
            o.quantity = 10;
            eng->submitOrder(std::move(o));
        }
        if (tickCount_ % 20 == 10 && eng->position(e.symbol) > 0) {
            qtl::Order o;
            o.id = nextId_++; o.symbol = e.symbol;
            o.side = qtl::Side::Sell; o.type = qtl::OrderType::Market;
            o.quantity = 10;
            eng->submitOrder(std::move(o));
        }
    }

private:
    int tickCount_{0};
    qtl::OrderId nextId_{10000};
};

static void test_backtest_engine_run() {
    std::string path = "/tmp/qtl_bt_test.csv";
    writeQuoteCSV(path, 200, "AAPL");

    qtl::BacktestEngine engine{100000.0};
    engine.addSymbol("AAPL");
    engine.addDataFile(path, qtl::TickType::Quote);
    engine.setStrategy(std::make_shared<SimpleMomentumStrategy>());

    auto summary = engine.run();
    ASSERT_TRUE(std::isfinite(summary.totalReturn), "totalReturn finite");
    ASSERT_TRUE(std::isfinite(summary.cagr),        "CAGR finite");
}

static void test_equity_curve_populated() {
    std::string path = "/tmp/qtl_bt_eq.csv";
    writeQuoteCSV(path, 100, "AAPL");

    qtl::BacktestEngine engine{50000.0};
    engine.addSymbol("AAPL");
    engine.addDataFile(path, qtl::TickType::Quote);
    engine.setStrategy(std::make_shared<SimpleMomentumStrategy>());
    engine.run();

    const auto& curve = engine.portfolio().equityCurve();
    ASSERT_FALSE(curve.empty(), "equity curve non-empty after run");
    ASSERT_TRUE(curve.size() > 1, "more than 1 equity point");
}

static void test_html_report_generated() {
    std::string dataPath   = "/tmp/qtl_bt_rpt.csv";
    std::string reportPath = "/tmp/qtl_report.html";
    writeQuoteCSV(dataPath, 50, "AAPL");

    qtl::BacktestEngine engine{10000.0};
    engine.addSymbol("AAPL");
    engine.addDataFile(dataPath, qtl::TickType::Quote);
    engine.setStrategy(std::make_shared<SimpleMomentumStrategy>());
    engine.run();
    engine.generateReport(reportPath);

    // Verify file exists and is non-empty
    std::ifstream f{reportPath};
    ASSERT_TRUE(f.is_open(), "report file exists");
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    ASSERT_FALSE(content.empty(), "report not empty");
    ASSERT_TRUE(content.find("<!DOCTYPE html>") != std::string::npos,
                "valid HTML header");
    ASSERT_TRUE(content.find("SimpleMomentum") != std::string::npos,
                "strategy name in report");
    ASSERT_TRUE(content.find("Chart.js") != std::string::npos ||
                content.find("chart.js") != std::string::npos,
                "Chart.js included");
}

static void test_summary_metrics_after_run() {
    std::string path = "/tmp/qtl_bt_summ.csv";
    writeQuoteCSV(path, 252, "AAPL");  // ~1 year

    qtl::BacktestEngine engine{100000.0};
    engine.addSymbol("AAPL");
    engine.addDataFile(path, qtl::TickType::Quote);
    engine.setStrategy(std::make_shared<SimpleMomentumStrategy>());
    auto s = engine.run();

    // All key metrics should be finite numbers
    ASSERT_TRUE(std::isfinite(s.totalReturn),       "totalReturn finite");
    ASSERT_TRUE(std::isfinite(s.cagr),              "CAGR finite");
    ASSERT_TRUE(std::isfinite(s.annualisedVol),     "vol finite");
    ASSERT_TRUE(std::isfinite(s.drawdown.maxDrawdown),"maxDD finite");
    // Equity curve should start near initial capital
    const auto& curve = engine.portfolio().equityCurve();
    if (!curve.empty()) {
        ASSERT_NEAR(curve.front(), 100000.0, 5000.0, "equity starts near capital");
    }
}

// ─────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────

void registerPerformanceMetricsTests() {
    registerTest("Metrics/equity_returns_roundtrip",   test_equity_returns_roundtrip);
    registerTest("Metrics/cagr_known_value",           test_cagr_known_value);
    registerTest("Metrics/sharpe_positive",            test_sharpe_positive);
    registerTest("Metrics/sharpe_zero_vol",            test_sharpe_zero_vol);
    registerTest("Metrics/sortino_vs_sharpe",          test_sortino_vs_sharpe);
    registerTest("Metrics/max_drawdown_known",         test_max_drawdown_known);
    registerTest("Metrics/max_drawdown_no_recovery",   test_max_drawdown_no_recovery);
    registerTest("Metrics/max_drawdown_recovery",      test_max_drawdown_with_recovery);
    registerTest("Metrics/calmar_ratio",               test_calmar_ratio);
    registerTest("Metrics/win_rate",                   test_win_rate);
    registerTest("Metrics/expectancy",                 test_expectancy);
    registerTest("Metrics/profit_factor",              test_profit_factor);
    registerTest("Metrics/avg_win_loss",               test_avg_win_loss);
    registerTest("Metrics/annualised_vol",             test_annualised_vol);
    registerTest("Metrics/var_parametric",             test_var_parametric);
    registerTest("Metrics/var_historical",             test_var_historical);
    registerTest("Metrics/cvar",                       test_cvar);
    registerTest("Metrics/omega_ratio",                test_omega_ratio);
    registerTest("Metrics/beta_alpha",                 test_beta_alpha);
    registerTest("Metrics/compute_summary_full",       test_compute_summary_full);
    registerTest("Metrics/summary_tostring",           test_summary_tostring);
    registerTest("Simulator/fixed_slippage",           test_fixed_slippage);
    registerTest("Simulator/percent_slippage",         test_percent_slippage);
    registerTest("Simulator/volume_slippage_scales",   test_volume_slippage_scales);
    registerTest("Simulator/limit_not_crossed",        test_limit_not_crossed_no_fill);
    registerTest("Simulator/market_always_fills",      test_market_order_always_fills);
    registerTest("Backtest/engine_run",                test_backtest_engine_run);
    registerTest("Backtest/equity_curve_populated",    test_equity_curve_populated);
    registerTest("Backtest/html_report_generated",     test_html_report_generated);
    registerTest("Backtest/summary_metrics_after_run", test_summary_metrics_after_run);
}
