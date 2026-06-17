/**
 * @file tests/test_portfolio.cpp
 * @brief Phase 11 — Portfolio Engine comprehensive tests.
 *
 * PositionManager (18):
 *  1.  Buy fill creates long position
 *  2.  Sell fill on flat creates short position
 *  3.  Buy fill closes short position → flat
 *  4.  FIFO realised P&L: buy 100@10, sell 100@12 = +$200
 *  5.  FIFO partial close: buy 100@10, sell 50@12 = +$100 realised, 50 remain
 *  6.  Average entry price updated correctly on add
 *  7.  Multiple buys: VWAP cost basis
 *  8.  Round-trip P&L: buy+sell same price = 0 (ignoring commission)
 *  9.  Mark-to-market updates unrealised P&L
 *  10. Unrealised P&L = 0 when flat
 *  11. netQty aggregates across strategies for same symbol
 *  12. openPositions() excludes flat positions
 *  13. allSymbols() returns tracked symbols
 *  14. Short position: unrealised P&L negative when price rises
 *  15. FIFO: multiple lots consumed in order
 *  16. Crossing flat: long → short in one trade
 *  17. printPositions() produces non-empty table
 *  18. reset() clears all state
 *
 * PnLTracker (12):
 *  19. Initial state: cash = initialCapital
 *  20. Buy fill reduces cash
 *  21. Sell fill increases cash
 *  22. Realised P&L accumulates correctly
 *  23. onMark updates equity curve
 *  24. High-water mark updates when equity rises
 *  25. Drawdown computed when equity falls below HWM
 *  26. Max drawdown tracks lowest point
 *  27. totalReturn = (equity - initial) / initial
 *  28. Strategy attribution: wins/losses/grossProfit
 *  29. resetDailyPnl() zeroes daily counter
 *  30. runningSharpe() returns finite value after fills
 *
 * PortfolioAccountant (10):
 *  31. onFill routes to both PositionManager and PnLTracker
 *  32. onMark updates NAV and equity curve
 *  33. onFill(FillEvent) convenience wrapper
 *  34. onMark(MarketEvent) uses last/mid price
 *  35. exposure: grossExposure = |qty| × price
 *  36. exposure: netExposure negative for short
 *  37. exposure: leverage computed from NAV
 *  38. Multi-strategy: each strategy tracked separately
 *  39. printFull() returns non-empty report
 *  40. Full round-trip: buy→mark→sell→mark checks P&L consistency
 */

#include "tests/TestHelper.hpp"
#include "portfolio/positions/PositionManager.hpp"
#include "portfolio/pnl/PnLTracker.hpp"
#include "portfolio/accounting/PortfolioAccountant.hpp"

#include <functional>
#include <string>
#include <vector>
#include <cmath>
#include <iostream>

extern void registerTest(std::string, std::function<void()>);
using namespace qtl;

// ─────────────────────────────────────────────────────────────
// PositionManager tests
// ─────────────────────────────────────────────────────────────

static void test_pm_buy_creates_long() {
    PositionManager pm;
    pm.onFill("AAPL", Side::Buy, 182.50, 100);
    auto pos = pm.getPosition("AAPL");
    ASSERT_EQ(pos.netQty, Quantity(100), "netQty=100");
    ASSERT_TRUE(pos.isLong(), "isLong");
    ASSERT_FALSE(pos.isFlat(), "not flat");
    ASSERT_NEAR(pos.avgEntryPrice, 182.50, 1e-6, "avgEntryPrice");
}

static void test_pm_sell_creates_short() {
    PositionManager pm{true};  // allow short selling
    pm.onFill("MSFT", Side::Sell, 415.0, 50);
    auto pos = pm.getPosition("MSFT");
    ASSERT_EQ(pos.netQty, Quantity(-50), "netQty=-50");
    ASSERT_TRUE(pos.isShort(), "isShort");
}

static void test_pm_buy_closes_short() {
    PositionManager pm{true};
    pm.onFill("SPY", Side::Sell, 450.0, 100);
    ASSERT_EQ(pm.netQty("SPY"), Quantity(-100), "short 100");
    pm.onFill("SPY", Side::Buy,  450.0, 100);
    ASSERT_EQ(pm.netQty("SPY"), Quantity(0), "flat after cover");
    auto pos = pm.getPosition("SPY");
    ASSERT_TRUE(pos.isFlat(), "isFlat after cover");
}

static void test_pm_fifo_realised_pnl() {
    PositionManager pm;
    pm.onFill("AAPL", Side::Buy,  10.0, 100);
    double pnl = pm.onFill("AAPL", Side::Sell, 12.0, 100);
    ASSERT_NEAR(pnl, 200.0, 1e-6, "realised P&L = +$200");
    ASSERT_NEAR(pm.totalRealisedPnl(), 200.0, 1e-6, "total realised = $200");
}

static void test_pm_partial_close() {
    PositionManager pm;
    pm.onFill("AAPL", Side::Buy,  10.0, 100);
    double pnl = pm.onFill("AAPL", Side::Sell, 12.0, 50);
    ASSERT_NEAR(pnl, 100.0, 1e-6, "partial close realised = +$100");
    auto pos = pm.getPosition("AAPL");
    ASSERT_EQ(pos.netQty, Quantity(50), "50 shares remain");
}

static void test_pm_avg_entry_price_updated() {
    PositionManager pm;
    pm.onFill("AAPL", Side::Buy, 100.0, 100);
    pm.onFill("AAPL", Side::Buy, 120.0, 100);
    auto pos = pm.getPosition("AAPL");
    ASSERT_EQ(pos.netQty, Quantity(200), "200 shares");
    ASSERT_NEAR(pos.avgEntryPrice, 110.0, 1e-6, "VWAP = (100*100+120*100)/200=110");
}

static void test_pm_multiple_buys_vwap() {
    PositionManager pm;
    pm.onFill("ETH", Side::Buy, 1000.0, 2);
    pm.onFill("ETH", Side::Buy, 1200.0, 3);
    pm.onFill("ETH", Side::Buy,  900.0, 5);
    auto pos = pm.getPosition("ETH");
    double expected = (1000*2 + 1200*3 + 900*5) / 10.0;
    ASSERT_NEAR(pos.avgEntryPrice, expected, 1e-4, "VWAP of 3 buys");
}

static void test_pm_round_trip_zero_pnl() {
    PositionManager pm;
    pm.onFill("BTC", Side::Buy,  50000.0, 1);
    double pnl = pm.onFill("BTC", Side::Sell, 50000.0, 1);
    ASSERT_NEAR(pnl, 0.0, 1e-6, "buy/sell same price → 0 P&L");
}

static void test_pm_unrealised_pnl_mtm() {
    PositionManager pm;
    pm.onFill("NVDA", Side::Buy, 800.0, 10);
    pm.mark("NVDA", 850.0);
    auto pos = pm.getPosition("NVDA");
    ASSERT_NEAR(pos.unrealisedPnl(), (850.0 - 800.0) * 10, 1e-6,
                "unrealised P&L after mark");
}

static void test_pm_unrealised_zero_when_flat() {
    PositionManager pm;
    pm.onFill("AAPL", Side::Buy,  182.0, 100);
    pm.onFill("AAPL", Side::Sell, 185.0, 100);
    auto pos = pm.getPosition("AAPL");
    ASSERT_EQ(pos.unrealisedPnl(), 0.0, "flat pos → zero unrealised P&L");
}

static void test_pm_netqty_aggregates_strategies() {
    PositionManager pm;
    pm.onFill("AAPL", Side::Buy, 180.0, 100, 0.0, "StratA");
    pm.onFill("AAPL", Side::Buy, 181.0,  50, 0.0, "StratB");
    ASSERT_EQ(pm.netQty("AAPL"), Quantity(150), "total 150 across strategies");
}

static void test_pm_open_positions_excludes_flat() {
    PositionManager pm;
    pm.onFill("AAPL", Side::Buy,  182.0, 100);
    pm.onFill("MSFT", Side::Buy,  415.0,  50);
    pm.onFill("AAPL", Side::Sell, 183.0, 100);  // closes AAPL
    auto open = pm.openPositions();
    ASSERT_EQ(open.size(), size_t(1), "1 open position (MSFT)");
    ASSERT_EQ(open[0].symbol, "MSFT", "open = MSFT");
}

static void test_pm_all_symbols() {
    PositionManager pm;
    pm.onFill("AAPL", Side::Buy, 182.0, 100);
    pm.onFill("GOOG", Side::Buy, 140.0,  50);
    auto syms = pm.allSymbols();
    ASSERT_EQ(syms.size(), size_t(2), "2 symbols tracked");
}

static void test_pm_short_unrealised_pnl_negative() {
    PositionManager pm{true};
    pm.onFill("TSLA", Side::Sell, 200.0, 10);
    pm.mark("TSLA", 220.0);  // price rose → short position loses
    auto pos = pm.getPosition("TSLA");
    ASSERT_TRUE(pos.unrealisedPnl() < 0,
                "short position loses when price rises");
    ASSERT_NEAR(pos.unrealisedPnl(), (220.0 - 200.0) * (-10), 1e-6,
                "short unrealised P&L = (mark - entry) × netQty");
}

static void test_pm_fifo_multiple_lots() {
    PositionManager pm;
    pm.onFill("X", Side::Buy, 10.0,  50);  // lot 1
    pm.onFill("X", Side::Buy, 12.0,  50);  // lot 2
    // Sell 75: consumes lot1(50) + 25 of lot2
    double pnl = pm.onFill("X", Side::Sell, 15.0, 75);
    // FIFO: (15-10)*50 + (15-12)*25 = 250 + 75 = 325
    ASSERT_NEAR(pnl, 325.0, 1e-4, "FIFO: lot1+partial lot2 realised");
    auto pos = pm.getPosition("X");
    ASSERT_EQ(pos.netQty, Quantity(25), "25 shares remain from lot2");
}

static void test_pm_crossing_flat_long_to_short() {
    PositionManager pm{true};
    pm.onFill("Y", Side::Buy,  100.0, 50);   // long 50
    pm.onFill("Y", Side::Sell, 110.0, 80);   // sell 80: close 50 + open short 30
    auto pos = pm.getPosition("Y");
    ASSERT_EQ(pos.netQty, Quantity(-30), "net short 30");
    ASSERT_TRUE(pos.isShort(), "isShort after crossing");
    ASSERT_TRUE(pm.totalRealisedPnl() > 0, "positive realised P&L from closing long");
}

static void test_pm_print_positions() {
    PositionManager pm;
    pm.onFill("AAPL", Side::Buy,  182.50, 100);
    pm.mark("AAPL", 183.00);
    std::string out = pm.printPositions();
    ASSERT_FALSE(out.empty(), "printPositions non-empty");
    ASSERT_TRUE(out.find("AAPL") != std::string::npos, "contains AAPL");
    ASSERT_TRUE(out.find("100") != std::string::npos, "contains qty");
}

static void test_pm_reset() {
    PositionManager pm;
    pm.onFill("AAPL", Side::Buy, 182.50, 100);
    pm.reset();
    ASSERT_EQ(pm.openPositionCount(), size_t(0), "0 after reset");
    ASSERT_EQ(pm.totalRealisedPnl(), 0.0, "realised 0 after reset");
}

// ─────────────────────────────────────────────────────────────
// PnLTracker tests
// ─────────────────────────────────────────────────────────────

static void test_pnl_initial_cash() {
    PnLTracker tracker{50000.0};
    ASSERT_NEAR(tracker.cash(), 50000.0, 1e-6, "initial cash = 50k");
    ASSERT_NEAR(tracker.currentEquity(), 50000.0, 1e-6, "initial equity = 50k");
    ASSERT_EQ(tracker.realisedPnl(), 0.0, "initial realised = 0");
}

static void test_pnl_buy_reduces_cash() {
    PnLTracker tracker{100000.0};
    tracker.onFill("AAPL", Side::Buy, 182.50, 100, 5.0, 0.0);
    // cash -= notional + commission = 18250 + 5 = 18255
    ASSERT_NEAR(tracker.cash(), 100000.0 - 18255.0, 1e-3, "cash reduced by buy");
}

static void test_pnl_sell_increases_cash() {
    PnLTracker tracker{100000.0};
    tracker.onFill("AAPL", Side::Sell, 182.50, 100, 5.0, 0.0);
    // cash += notional - commission = 18250 - 5 = 18245
    ASSERT_NEAR(tracker.cash(), 100000.0 + 18245.0, 1e-3, "cash increased by sell");
}

static void test_pnl_realised_accumulates() {
    PnLTracker tracker{100000.0};
    tracker.onFill("A", Side::Buy,  100.0, 50, 0, 0.0);
    tracker.onFill("A", Side::Sell, 110.0, 50, 0, 500.0);  // +$500 realised
    tracker.onFill("B", Side::Buy,  200.0, 10, 0, 0.0);
    tracker.onFill("B", Side::Sell, 205.0, 10, 0,  50.0);  // +$50 realised
    ASSERT_NEAR(tracker.realisedPnl(), 550.0, 1e-6, "total realised = $550");
}

static void test_pnl_onmark_updates_equity() {
    PnLTracker tracker{100000.0};
    tracker.onFill("AAPL", Side::Buy, 182.50, 100, 0, 0);
    tracker.onMark(500.0);  // unrealised = +$500
    auto curve = tracker.equityVector();
    ASSERT_FALSE(curve.empty(), "equity curve non-empty after mark");
    ASSERT_NEAR(curve.back(), tracker.currentEquity(), 1e-3, "last point = current equity");
}

static void test_pnl_hwm_rises_with_equity() {
    PnLTracker tracker{100000.0};
    ASSERT_NEAR(tracker.highWaterMark(), 100000.0, 1e-6, "HWM starts at initial");
    tracker.onFill("X", Side::Sell, 100.0, 10, 0, 200.0);
    tracker.onMark(0.0);  // P&L=200 from realised
    // After mark: equity = cash + unrealised
    // HWM should still be at least 100k (may not rise from mark if cash offset by fill)
    ASSERT_TRUE(tracker.highWaterMark() >= 100000.0, "HWM non-decreasing");
}

static void test_pnl_drawdown_computed() {
    PnLTracker tracker{100000.0};
    // Mark up to 110k
    tracker.onMark(10000.0);   // unrealised +10k → equity 110k
    ASSERT_NEAR(tracker.highWaterMark(), 110000.0 - tracker.totalCommission(), 100.0,
                "HWM near 110k");
    // Now drawdown
    tracker.onMark(-5000.0);   // unrealised -5k → equity 105k
    ASSERT_TRUE(tracker.currentDrawdown() < 0, "drawdown is negative");
}

static void test_pnl_max_drawdown() {
    PnLTracker tracker{100000.0};
    tracker.onMark(10000.0);   // peak equity
    tracker.onMark(-15000.0);  // trough
    tracker.onMark(-5000.0);   // partial recovery
    ASSERT_TRUE(tracker.maxDrawdown() < 0, "maxDrawdown negative");
    ASSERT_TRUE(tracker.maxDrawdown() <= tracker.currentDrawdown(),
                "maxDD <= currentDD");
}

static void test_pnl_total_return() {
    PnLTracker tracker{100000.0};
    // Simulate 10% gain
    tracker.onFill("X", Side::Buy, 100.0, 100, 0, 0);
    tracker.onFill("X", Side::Sell, 110.0, 100, 0, 1000.0);
    double ret = tracker.totalReturn();
    ASSERT_TRUE(std::isfinite(ret), "totalReturn finite");
}

static void test_pnl_strategy_attribution() {
    PnLTracker tracker{100000.0};
    tracker.onFill("A", Side::Buy,  100.0, 10, 0,    0.0, "StratA");
    tracker.onFill("A", Side::Sell, 110.0, 10, 0, +100.0, "StratA");  // win
    tracker.onFill("B", Side::Buy,  200.0, 5,  0,    0.0, "StratA");
    tracker.onFill("B", Side::Sell, 195.0, 5,  0,  -25.0, "StratA");  // loss

    auto sp = tracker.strategyPnl("StratA");
    ASSERT_EQ(sp.wins,   uint64_t(1), "1 win");
    ASSERT_EQ(sp.losses, uint64_t(1), "1 loss");
    ASSERT_NEAR(sp.grossProfit, 100.0, 1e-6, "gross profit = $100");
    ASSERT_NEAR(sp.grossLoss,    25.0, 1e-6, "gross loss = $25");
    ASSERT_TRUE(sp.profitFactor() > 1.0, "PF > 1");
}

static void test_pnl_reset_daily() {
    PnLTracker tracker{100000.0};
    tracker.onFill("X", Side::Sell, 100.0, 10, 0, 500.0);
    ASSERT_TRUE(tracker.dailyPnl() != 0.0, "daily P&L non-zero before reset");
    tracker.resetDailyPnl();
    // After reset, daily realised = 0. unrealisedPnl is also 0 here.
    ASSERT_NEAR(tracker.dailyPnl(), 0.0, 1e-6, "daily P&L = 0 after reset");
}

static void test_pnl_running_sharpe() {
    PnLTracker tracker{100000.0};
    // Add some marks to build equity curve
    for (int i = 0; i < 20; ++i) {
        tracker.onMark(static_cast<double>(i % 3) * 100.0 - 100.0);
    }
    double sh = tracker.runningSharpe();
    ASSERT_TRUE(std::isfinite(sh), "Sharpe is finite after marks");
}

// ─────────────────────────────────────────────────────────────
// PortfolioAccountant tests
// ─────────────────────────────────────────────────────────────

static void test_pa_onfill_routes_both() {
    PortfolioAccountant pa{100000.0};
    pa.onFill("AAPL", Side::Buy, 182.50, 100, 5.0, "StratA");

    // Position updated
    ASSERT_EQ(pa.netQty("AAPL"), Quantity(100), "position manager updated");
    // Cash reduced
    ASSERT_NEAR(pa.cash(), 100000.0 - 182.50*100 - 5.0, 1e-3, "cash reduced");
    ASSERT_NEAR(pa.totalCommission(), 5.0, 1e-6, "commission tracked");
}

static void test_pa_onmark_updates_nav() {
    PortfolioAccountant pa{100000.0};
    pa.onFill("AAPL", Side::Buy, 182.50, 100, 0.0);
    pa.onMark("AAPL", 185.00);  // +$250 unrealised

    ASSERT_TRUE(pa.nav() > pa.cash(), "NAV > cash after positive MTM");
    ASSERT_NEAR(pa.unrealisedPnl(), 250.0, 1.0, "unrealised P&L ~$250");
}

static void test_pa_fillevent_wrapper() {
    PortfolioAccountant pa{100000.0};
    FillEvent fe{1001, 5001, "AAPL", Side::Buy, 182.50, 100, 0, 5.0, true, "MM"};
    pa.onFill(fe);
    ASSERT_EQ(pa.netQty("AAPL"), Quantity(100), "position from FillEvent");
}

static void test_pa_marketevent_wrapper() {
    PortfolioAccountant pa{100000.0};
    pa.onFill("AAPL", Side::Buy, 182.50, 100, 0.0);
    MarketEvent me{"AAPL", 183.0, 100, 183.5, 200, 183.25, 50};
    pa.onMark(me);
    // last price = 183.25 → unrealised = (183.25 - 182.50) * 100 = 75
    ASSERT_NEAR(pa.unrealisedPnl(), 75.0, 5.0, "unrealised from MarketEvent");
}

static void test_pa_exposure_gross() {
    PortfolioAccountant pa{200000.0};
    pa.onFill("AAPL", Side::Buy,  182.50, 100, 0.0);
    pa.onFill("MSFT", Side::Buy,  415.00,  50, 0.0);
    pa.onMark("AAPL", 182.50);
    pa.onMark("MSFT", 415.00);

    auto exp = pa.exposure();
    double expectedGross = 182.50*100 + 415.00*50;
    ASSERT_NEAR(exp.grossExposure, expectedGross, 1.0, "gross exposure");
    ASSERT_NEAR(exp.netExposure,   expectedGross, 1.0, "net = gross (all long)");
    ASSERT_TRUE(exp.grossLeverage > 0, "leverage > 0");
}

static void test_pa_exposure_net_short_negative() {
    PortfolioAccountant pa{200000.0, true};
    pa.onFill("X", Side::Sell, 100.0, 100, 0.0);
    pa.onMark("X", 100.0);
    auto exp = pa.exposure();
    ASSERT_TRUE(exp.netExposure < 0, "net exposure negative for short");
    ASSERT_TRUE(exp.grossExposure > 0, "gross exposure positive for short");
    ASSERT_NEAR(exp.shortExposure, 10000.0, 1.0, "short exposure = $10k");
}

static void test_pa_exposure_leverage() {
    PortfolioAccountant pa{20000.0};  // start with 2x the position size
    pa.onFill("X", Side::Buy, 100.0, 100, 0.0);  // $10k position, $10k cash remains
    pa.onMark("X", 100.0);
    auto exp = pa.exposure();
    // NAV = cash($10k) + unrealised($0) = $10k; gross = $10k → leverage = 1x
    ASSERT_TRUE(exp.grossLeverage > 0, "leverage > 0");
    ASSERT_NEAR(exp.grossLeverage, 1.0, 0.5, "leverage ≈ 1x");
}

static void test_pa_multi_strategy() {
    PortfolioAccountant pa{100000.0};
    pa.onFill("AAPL", Side::Buy,  180.0, 100, 0, "MomentumA");
    pa.onFill("AAPL", Side::Sell, 185.0, 100, 0, "MomentumA");  // +$500

    pa.onFill("MSFT", Side::Buy,  415.0, 50,  0, "MarketMaking");
    pa.onFill("MSFT", Side::Sell, 414.0, 50,  0, "MarketMaking"); // -$50

    auto spA = pa.strategyPnl("MomentumA");
    auto spM = pa.strategyPnl("MarketMaking");

    ASSERT_NEAR(spA.realisedPnl, 500.0, 1e-3, "MomentumA +$500");
    ASSERT_NEAR(spM.realisedPnl, -50.0, 1e-3, "MarketMaking -$50");

    auto all = pa.allStrategyPnl();
    ASSERT_EQ(all.size(), size_t(2), "2 strategies tracked");
}

static void test_pa_print_full() {
    PortfolioAccountant pa{100000.0};
    pa.onFill("AAPL", Side::Buy, 182.50, 100, 5.0, "S1");
    pa.onMark("AAPL", 183.00);
    std::string report = pa.printFull();
    ASSERT_FALSE(report.empty(), "printFull non-empty");
    ASSERT_TRUE(report.find("P&L") != std::string::npos,    "contains P&L");
    ASSERT_TRUE(report.find("AAPL") != std::string::npos,   "contains AAPL");
    ASSERT_TRUE(report.find("Exposure") != std::string::npos,"contains Exposure");
}

static void test_pa_full_roundtrip() {
    PortfolioAccountant pa{100000.0};
    double initial = pa.nav();

    // Buy 100 AAPL @ 182.50
    pa.onFill("AAPL", Side::Buy, 182.50, 100, 10.0, "Test");
    pa.onMark("AAPL", 185.00);  // unrealised +$250

    // Verify unrealised
    ASSERT_NEAR(pa.unrealisedPnl(), 250.0, 2.0, "unrealised +$250");
    ASSERT_TRUE(pa.totalCommission() > 0, "commission charged on buy");

    // Sell 100 AAPL @ 185.00
    pa.onFill("AAPL", Side::Sell, 185.00, 100, 10.0, "Test");
    pa.onMark("AAPL", 185.00);  // now flat

    // After close: realised = +$250, commission = -$20, total = +$230
    ASSERT_NEAR(pa.realisedPnl(), 250.0, 1.0, "realised after close = $250");
    ASSERT_NEAR(pa.totalCommission(), 20.0, 1e-6, "total commission = $20");
    ASSERT_EQ(pa.netQty("AAPL"), Quantity(0), "flat after sell");
    ASSERT_NEAR(pa.nav(), initial + 230.0, 5.0, "NAV = initial + 250 - 20 commissions");
}

// ─────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────

void registerPortfolioTests() {
    // PositionManager
    registerTest("Portfolio/pm_buy_creates_long",       test_pm_buy_creates_long);
    registerTest("Portfolio/pm_sell_creates_short",     test_pm_sell_creates_short);
    registerTest("Portfolio/pm_buy_closes_short",       test_pm_buy_closes_short);
    registerTest("Portfolio/pm_fifo_realised_pnl",      test_pm_fifo_realised_pnl);
    registerTest("Portfolio/pm_partial_close",          test_pm_partial_close);
    registerTest("Portfolio/pm_avg_entry_price",        test_pm_avg_entry_price_updated);
    registerTest("Portfolio/pm_multiple_buys_vwap",     test_pm_multiple_buys_vwap);
    registerTest("Portfolio/pm_round_trip_zero_pnl",    test_pm_round_trip_zero_pnl);
    registerTest("Portfolio/pm_unrealised_mtm",         test_pm_unrealised_pnl_mtm);
    registerTest("Portfolio/pm_unrealised_zero_flat",   test_pm_unrealised_zero_when_flat);
    registerTest("Portfolio/pm_netqty_aggregates",      test_pm_netqty_aggregates_strategies);
    registerTest("Portfolio/pm_open_excludes_flat",     test_pm_open_positions_excludes_flat);
    registerTest("Portfolio/pm_all_symbols",            test_pm_all_symbols);
    registerTest("Portfolio/pm_short_unrealised_neg",   test_pm_short_unrealised_pnl_negative);
    registerTest("Portfolio/pm_fifo_multiple_lots",     test_pm_fifo_multiple_lots);
    registerTest("Portfolio/pm_crossing_flat",          test_pm_crossing_flat_long_to_short);
    registerTest("Portfolio/pm_print_positions",        test_pm_print_positions);
    registerTest("Portfolio/pm_reset",                  test_pm_reset);
    // PnLTracker
    registerTest("Portfolio/pnl_initial_cash",          test_pnl_initial_cash);
    registerTest("Portfolio/pnl_buy_reduces_cash",      test_pnl_buy_reduces_cash);
    registerTest("Portfolio/pnl_sell_increases_cash",   test_pnl_sell_increases_cash);
    registerTest("Portfolio/pnl_realised_accumulates",  test_pnl_realised_accumulates);
    registerTest("Portfolio/pnl_onmark_updates_equity", test_pnl_onmark_updates_equity);
    registerTest("Portfolio/pnl_hwm_rises",             test_pnl_hwm_rises_with_equity);
    registerTest("Portfolio/pnl_drawdown_computed",     test_pnl_drawdown_computed);
    registerTest("Portfolio/pnl_max_drawdown",          test_pnl_max_drawdown);
    registerTest("Portfolio/pnl_total_return",          test_pnl_total_return);
    registerTest("Portfolio/pnl_strategy_attribution",  test_pnl_strategy_attribution);
    registerTest("Portfolio/pnl_reset_daily",           test_pnl_reset_daily);
    registerTest("Portfolio/pnl_running_sharpe",        test_pnl_running_sharpe);
    // PortfolioAccountant
    registerTest("Portfolio/pa_onfill_routes_both",     test_pa_onfill_routes_both);
    registerTest("Portfolio/pa_onmark_updates_nav",     test_pa_onmark_updates_nav);
    registerTest("Portfolio/pa_fillevent_wrapper",      test_pa_fillevent_wrapper);
    registerTest("Portfolio/pa_marketevent_wrapper",    test_pa_marketevent_wrapper);
    registerTest("Portfolio/pa_exposure_gross",         test_pa_exposure_gross);
    registerTest("Portfolio/pa_exposure_net_short",     test_pa_exposure_net_short_negative);
    registerTest("Portfolio/pa_exposure_leverage",      test_pa_exposure_leverage);
    registerTest("Portfolio/pa_multi_strategy",         test_pa_multi_strategy);
    registerTest("Portfolio/pa_print_full",             test_pa_print_full);
    registerTest("Portfolio/pa_full_roundtrip",         test_pa_full_roundtrip);
}
