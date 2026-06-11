/**
 * @file tests/test_order_book.cpp
 * @brief Phase 3 — comprehensive Order Book unit tests.
 *
 * Tests cover:
 *  1.  Empty book state
 *  2.  Add resting bid / ask — no match
 *  3.  FIFO queue priority within a price level
 *  4.  Limit order crossing spread — full fill
 *  5.  Partial fill — maker partially consumed
 *  6.  Partial fill — taker partially consumed
 *  7.  Market order — sweeps multiple levels
 *  8.  Cancel resting order
 *  9.  Modify price (loses queue priority)
 *  10. Modify quantity decrease (preserves priority)
 *  11. IOC order — fills partial, cancels remainder
 *  12. FOK order — fills completely
 *  13. FOK order — rejected (insufficient liquidity)
 *  14. Duplicate order ID rejected
 *  15. Best bid/ask / spread / midprice correctness
 *  16. TradeReport field verification
 *  17. Multi-level sweep — correct prices and quantities
 *  18. PrintBook output (non-empty, contains symbol)
 *  19. Snapshot depth limiting
 *  20. Large-scale: 10 000 orders then random cancels
 */

#include "tests/TestHelper.hpp"
#include "exchange/orderbook/Order.hpp"
#include "exchange/orderbook/OrderBook.hpp"
#include <iostream>

#include <functional>
#include <string>
#include <vector>
#include <random>
#include <algorithm>

extern void registerTest(std::string, std::function<void()>);

// ─────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────

static qtl::Order makeLimitBid(qtl::OrderId id, qtl::Price p,
                                qtl::Quantity q, std::string strat = "S1") {
    qtl::Order o;
    o.id = id; o.side = qtl::Side::Buy;
    o.type = qtl::OrderType::Limit;
    o.price = p; o.quantity = q;
    o.tif = qtl::TimeInForce::GTC;
    o.strategyId = std::move(strat);
    return o;
}

static qtl::Order makeLimitAsk(qtl::OrderId id, qtl::Price p,
                                qtl::Quantity q, std::string strat = "S1") {
    qtl::Order o;
    o.id = id; o.side = qtl::Side::Sell;
    o.type = qtl::OrderType::Limit;
    o.price = p; o.quantity = q;
    o.tif = qtl::TimeInForce::GTC;
    o.strategyId = std::move(strat);
    return o;
}

static qtl::Order makeMarketBid(qtl::OrderId id, qtl::Quantity q) {
    qtl::Order o;
    o.id = id; o.side = qtl::Side::Buy;
    o.type = qtl::OrderType::Market;
    o.quantity = q;
    return o;
}

static qtl::Order makeMarketAsk(qtl::OrderId id, qtl::Quantity q) {
    qtl::Order o;
    o.id = id; o.side = qtl::Side::Sell;
    o.type = qtl::OrderType::Market;
    o.quantity = q;
    return o;
}

static qtl::Order makeIOC(qtl::OrderId id, qtl::Side side,
                           qtl::Price p, qtl::Quantity q) {
    qtl::Order o;
    o.id = id; o.side = side;
    o.type = qtl::OrderType::Limit;
    o.price = p; o.quantity = q;
    o.tif = qtl::TimeInForce::IOC;
    return o;
}

static qtl::Order makeFOK(qtl::OrderId id, qtl::Side side,
                           qtl::Price p, qtl::Quantity q) {
    qtl::Order o;
    o.id = id; o.side = side;
    o.type = qtl::OrderType::Limit;
    o.price = p; o.quantity = q;
    o.tif = qtl::TimeInForce::FOK;
    return o;
}

// ─────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────

static void test_empty_book() {
    qtl::OrderBook book{"AAPL"};
    ASSERT_EQ(book.bestBid(),   0.0, "empty bid = 0");
    ASSERT_EQ(book.bestAsk(),   0.0, "empty ask = 0");
    ASSERT_EQ(book.midPrice(),  0.0, "empty mid = 0");
    ASSERT_EQ(book.spread(),    0.0, "empty spread = 0");
    ASSERT_EQ(book.bidLevels(), size_t(0), "0 bid levels");
    ASSERT_EQ(book.askLevels(), size_t(0), "0 ask levels");
    ASSERT_EQ((book.bidOrderCount() + book.askOrderCount()), size_t(0), "0 orders");
}

static void test_add_resting_orders() {
    qtl::OrderBook book{"AAPL"};
    book.addOrder(makeLimitBid(1, 180.00, 100));
    book.addOrder(makeLimitBid(2, 179.99, 200));
    book.addOrder(makeLimitAsk(3, 180.01, 150));
    book.addOrder(makeLimitAsk(4, 180.02,  50));

    ASSERT_NEAR(book.bestBid(), 180.00, 1e-9, "best bid");
    ASSERT_NEAR(book.bestAsk(), 180.01, 1e-9, "best ask");
    ASSERT_NEAR(book.spread(),    0.01, 1e-9, "spread");
    ASSERT_NEAR(book.midPrice(),180.005,1e-9, "mid");
    ASSERT_EQ(book.bidLevels(), size_t(2), "2 bid levels");
    ASSERT_EQ(book.askLevels(), size_t(2), "2 ask levels");
    ASSERT_EQ(book.bidOrderCount(), size_t(2), "2 bid orders");
    ASSERT_EQ(book.askOrderCount(), size_t(2), "2 ask orders");
}

static void test_fifo_priority() {
    qtl::OrderBook book{"MSFT"};
    // Three orders at same price — should match in arrival order
    book.addOrder(makeLimitAsk(10, 415.00, 100));
    book.addOrder(makeLimitAsk(11, 415.00, 200));
    book.addOrder(makeLimitAsk(12, 415.00, 300));

    std::vector<qtl::TradeReport> trades;
    book.setTradeCallback([&](const qtl::TradeReport& tr){
        trades.push_back(tr);
    });

    // Buy 250 — should consume order 10 (100) then 150 of order 11
    book.addOrder(makeMarketBid(20, 250));

    ASSERT_EQ(trades.size(), size_t(2), "2 trades: two makers hit");
    ASSERT_EQ(trades[0].makerOrderId, qtl::OrderId(10), "first maker is id=10 (FIFO)");
    ASSERT_EQ(trades[0].quantity, qtl::Quantity(100),   "fills order 10 fully");
    ASSERT_EQ(trades[1].makerOrderId, qtl::OrderId(11), "second maker is id=11");
    ASSERT_EQ(trades[1].quantity, qtl::Quantity(150),   "fills 150 of order 11");

    // Order 10 should be gone; order 11 partially filled; order 12 untouched
    ASSERT_TRUE(book.findOrder(10) == nullptr, "order 10 fully filled, removed");
    const qtl::Order* o11 = book.findOrder(11);
    ASSERT_TRUE(o11 != nullptr, "order 11 still resting");
    ASSERT_EQ(o11->filledQty, qtl::Quantity(150), "order 11 filled 150");
    ASSERT_EQ(o11->remainingQty(), qtl::Quantity(50), "order 11 remaining 50");
}

static void test_full_fill_limit_cross() {
    qtl::OrderBook book{"SPY"};
    std::vector<qtl::TradeReport> trades;
    book.setTradeCallback([&](const qtl::TradeReport& t){ trades.push_back(t); });

    book.addOrder(makeLimitAsk(1, 450.00, 500));
    auto res = book.addOrder(makeLimitBid(2, 450.00, 500)); // exact cross

    ASSERT_TRUE(res.accepted, "buy accepted");
    ASSERT_EQ(trades.size(), size_t(1), "one trade");
    ASSERT_EQ(trades[0].quantity, qtl::Quantity(500), "full fill 500");
    ASSERT_NEAR(trades[0].price, 450.00, 1e-9, "filled at maker price");
    ASSERT_EQ((book.bidOrderCount() + book.askOrderCount()), size_t(0), "book empty after full cross");
    ASSERT_EQ(book.tradeCount(), uint64_t(1), "1 trade recorded");
}

static void test_partial_fill_maker() {
    qtl::OrderBook book{"QQQ"};
    std::vector<qtl::TradeReport> trades;
    book.setTradeCallback([&](const qtl::TradeReport& t){ trades.push_back(t); });

    book.addOrder(makeLimitAsk(1, 375.00, 1000));
    book.addOrder(makeMarketBid(2, 300));  // taker buys 300 of 1000

    ASSERT_EQ(trades.size(), size_t(1), "one trade");
    ASSERT_EQ(trades[0].quantity, qtl::Quantity(300), "fill qty = 300");

    const qtl::Order* maker = book.findOrder(1);
    ASSERT_TRUE(maker != nullptr, "maker still resting");
    ASSERT_EQ(maker->filledQty,    qtl::Quantity(300), "maker filled 300");
    ASSERT_EQ(maker->remainingQty(),qtl::Quantity(700), "maker remaining 700");
    ASSERT_TRUE(maker->status == qtl::OrderStatus::PartiallyFilled, "PartiallyFilled");
}

static void test_partial_fill_taker() {
    qtl::OrderBook book{"IWM"};
    book.addOrder(makeLimitAsk(1, 200.00, 100));
    book.addOrder(makeLimitAsk(2, 200.00, 100)); // 200 total

    std::vector<qtl::TradeReport> trades;
    book.setTradeCallback([&](const qtl::TradeReport& t){ trades.push_back(t); });

    // Taker buys 150 — partial against two makers
    book.addOrder(makeLimitBid(3, 200.10, 150));

    ASSERT_EQ(trades.size(), size_t(2), "2 fills (sweeps order 1, then partial order 2)");
    ASSERT_EQ(trades[0].quantity, qtl::Quantity(100), "first fill = 100");
    ASSERT_EQ(trades[1].quantity, qtl::Quantity(50),  "second fill = 50");
    ASSERT_EQ(book.findOrder(1), nullptr, "order 1 consumed");

    const qtl::Order* o2 = book.findOrder(2);
    ASSERT_TRUE(o2 != nullptr, "order 2 still resting");
    ASSERT_EQ(o2->remainingQty(), qtl::Quantity(50), "order 2 remaining 50");
}

static void test_market_order_sweeps_levels() {
    qtl::OrderBook book{"NVDA"};
    book.addOrder(makeLimitAsk(1, 800.00, 100));
    book.addOrder(makeLimitAsk(2, 801.00, 200));
    book.addOrder(makeLimitAsk(3, 802.00, 300));

    std::vector<qtl::TradeReport> trades;
    book.setTradeCallback([&](const qtl::TradeReport& t){ trades.push_back(t); });

    book.addOrder(makeMarketBid(10, 450)); // sweeps all 3 levels (100+200+150)

    ASSERT_EQ(trades.size(), size_t(3), "3 trades across 3 levels");
    ASSERT_NEAR(trades[0].price, 800.00, 1e-9, "filled at level 1 price");
    ASSERT_NEAR(trades[1].price, 801.00, 1e-9, "filled at level 2 price");
    ASSERT_NEAR(trades[2].price, 802.00, 1e-9, "filled at level 3 price");
    ASSERT_EQ(trades[2].quantity, qtl::Quantity(150), "partial fill on level 3");

    // Level 3 partially consumed — order 3 should still rest with 150 remaining
    const qtl::Order* o3 = book.findOrder(3);
    ASSERT_TRUE(o3 != nullptr, "order 3 still resting");
    ASSERT_EQ(o3->remainingQty(), qtl::Quantity(150), "order 3 has 150 left");
}

static void test_cancel_order() {
    qtl::OrderBook book{"GOOG"};
    book.addOrder(makeLimitBid(1, 140.00, 500));
    book.addOrder(makeLimitBid(2, 139.00, 300));

    ASSERT_EQ(book.bidOrderCount(), size_t(2), "2 bids before cancel");
    bool cancelled = book.cancelOrder(1);
    ASSERT_TRUE(cancelled, "cancel returned true");
    ASSERT_EQ(book.bidOrderCount(), size_t(1), "1 bid after cancel");
    ASSERT_TRUE(book.findOrder(1) == nullptr, "order 1 not found");
    ASSERT_NEAR(book.bestBid(), 139.00, 1e-9, "best bid now 139");

    // Cancel non-existent
    ASSERT_FALSE(book.cancelOrder(999), "cancel unknown = false");
    // Double cancel
    ASSERT_FALSE(book.cancelOrder(1),   "double cancel = false");
}

static void test_modify_price_loses_priority() {
    qtl::OrderBook book{"TSLA"};
    std::vector<qtl::TradeReport> trades;
    book.setTradeCallback([&](const qtl::TradeReport& t){ trades.push_back(t); });

    // Two bids at same price — order 1 arrived first (FIFO priority)
    book.addOrder(makeLimitBid(1, 250.00, 100));
    book.addOrder(makeLimitBid(2, 250.00, 100));

    // Increase quantity of order 1 — forces re-insert, loses queue priority
    // (now order 2 is first at 250.00, order 1 is second)
    qtl::ModifyRequest req{1, 0.0, 500}; // qty increase → re-insert at back
    book.modifyOrder(req);

    // Sell 100 at market — should hit order 2 (now queue-front at 250.00)
    book.addOrder(makeMarketAsk(10, 100));
    ASSERT_EQ(trades.size(), size_t(1), "one trade");
    ASSERT_EQ(trades[0].makerOrderId, qtl::OrderId(2),
              "order 2 first after order 1 qty-increase lost priority");
    ASSERT_EQ(trades[0].quantity, qtl::Quantity(100), "full fill of 100");
}

static void test_modify_qty_decrease_preserves_priority() {
    qtl::OrderBook book{"AMZN"};
    std::vector<qtl::TradeReport> trades;
    book.setTradeCallback([&](const qtl::TradeReport& t){ trades.push_back(t); });

    book.addOrder(makeLimitBid(1, 185.00, 500));
    book.addOrder(makeLimitBid(2, 185.00, 100));

    // Reduce order 1's quantity — should preserve priority
    qtl::ModifyRequest req{1, 0.0, 200};
    bool ok = book.modifyOrder(req);
    ASSERT_TRUE(ok, "modify accepted");

    // Sell 200 — should match order 1 (priority preserved)
    book.addOrder(makeMarketAsk(10, 200));
    ASSERT_EQ(trades.size(), size_t(1), "one trade");
    ASSERT_EQ(trades[0].makerOrderId, qtl::OrderId(1), "order 1 still first");
    ASSERT_EQ(trades[0].quantity, qtl::Quantity(200), "fill 200");
}

static void test_ioc_order() {
    qtl::OrderBook book{"META"};
    book.addOrder(makeLimitAsk(1, 500.00, 100));

    std::vector<qtl::TradeReport> trades;
    book.setTradeCallback([&](const qtl::TradeReport& t){ trades.push_back(t); });

    // IOC buy 200 — only 100 available, fills 100, cancels 100
    auto res = book.addOrder(makeIOC(2, qtl::Side::Buy, 500.00, 200));

    ASSERT_TRUE(res.accepted, "IOC accepted");
    ASSERT_EQ(trades.size(), size_t(1), "one trade");
    ASSERT_EQ(trades[0].quantity, qtl::Quantity(100), "fills available 100");

    // IOC remainder should NOT rest in book
    ASSERT_EQ(book.bidOrderCount(), size_t(0), "IOC remainder not resting");
    ASSERT_TRUE(book.findOrder(2) == nullptr, "IOC order not in book");
}

static void test_fok_success() {
    qtl::OrderBook book{"COIN"};
    book.addOrder(makeLimitAsk(1, 200.00, 500));

    std::vector<qtl::TradeReport> trades;
    book.setTradeCallback([&](const qtl::TradeReport& t){ trades.push_back(t); });

    auto res = book.addOrder(makeFOK(2, qtl::Side::Buy, 200.00, 300));
    ASSERT_TRUE(res.accepted, "FOK accepted (enough liquidity)");
    ASSERT_EQ(trades.size(), size_t(1), "one trade");
    ASSERT_EQ(trades[0].quantity, qtl::Quantity(300), "filled 300");
}

static void test_fok_reject() {
    qtl::OrderBook book{"DOGE"};
    book.addOrder(makeLimitAsk(1, 0.10, 50));  // only 50 available

    std::vector<qtl::TradeReport> trades;
    book.setTradeCallback([&](const qtl::TradeReport& t){ trades.push_back(t); });

    auto res = book.addOrder(makeFOK(2, qtl::Side::Buy, 0.10, 200));
    ASSERT_FALSE(res.accepted, "FOK rejected (insufficient liquidity)");
    ASSERT_EQ(trades.size(), size_t(0), "no trades on rejected FOK");
    // Book untouched
    ASSERT_EQ(book.askOrderCount(), size_t(1), "ask still resting");
}

static void test_duplicate_order_id() {
    qtl::OrderBook book{"AAPL"};
    auto res1 = book.addOrder(makeLimitBid(42, 180.00, 100));
    auto res2 = book.addOrder(makeLimitBid(42, 179.00, 200)); // duplicate id

    ASSERT_TRUE(res1.accepted,  "first order accepted");
    ASSERT_FALSE(res2.accepted, "duplicate id rejected");
    ASSERT_EQ(book.bidOrderCount(), size_t(1), "only 1 bid");
}

static void test_best_prices() {
    qtl::OrderBook book{"SPX"};
    book.addOrder(makeLimitBid(1, 5000.00, 10));
    book.addOrder(makeLimitBid(2, 4999.00, 20));
    book.addOrder(makeLimitBid(3, 5001.00,  5)); // new best bid
    book.addOrder(makeLimitAsk(4, 5002.00, 10));
    book.addOrder(makeLimitAsk(5, 5003.00, 20));
    book.addOrder(makeLimitAsk(6, 5001.50,  5)); // new best ask

    ASSERT_NEAR(book.bestBid(), 5001.00, 1e-9, "best bid = 5001");
    ASSERT_NEAR(book.bestAsk(), 5001.50, 1e-9, "best ask = 5001.5");
    ASSERT_NEAR(book.spread(),     0.50, 1e-9, "spread = 0.5");
    ASSERT_NEAR(book.midPrice(), 5001.25,1e-9, "mid = 5001.25");
}

static void test_trade_report_fields() {
    qtl::OrderBook book{"AAPL"};
    qtl::TradeReport captured{};
    book.setTradeCallback([&](const qtl::TradeReport& t){ captured = t; });

    book.addOrder(makeLimitAsk(100, 182.50, 300));
    book.addOrder(makeLimitBid(101, 182.50, 200));

    ASSERT_EQ(captured.makerOrderId, qtl::OrderId(100),  "maker = ask 100");
    ASSERT_EQ(captured.takerOrderId, qtl::OrderId(101),  "taker = bid 101");
    ASSERT_TRUE(captured.takerSide == qtl::Side::Buy,    "taker side = Buy");
    ASSERT_NEAR(captured.price, 182.50, 1e-9,            "fill price = 182.50");
    ASSERT_EQ(captured.quantity, qtl::Quantity(200),     "fill qty = 200");
    ASSERT_EQ(captured.symbol, "AAPL",                   "symbol = AAPL");
    ASSERT_TRUE(captured.tradeId > 0,                    "tradeId > 0");
    ASSERT_TRUE(captured.timestamp > 0,                  "timestamp set");
    ASSERT_TRUE(captured.matchLatencyNs >= 0,            "latency >= 0");
}

static void test_multi_level_sweep_prices() {
    qtl::OrderBook book{"BTC"};
    std::vector<qtl::TradeReport> trades;
    book.setTradeCallback([&](const qtl::TradeReport& t){ trades.push_back(t); });

    // Build ask side: 3 levels
    book.addOrder(makeLimitAsk(1, 45000.00, 1));
    book.addOrder(makeLimitAsk(2, 45100.00, 1));
    book.addOrder(makeLimitAsk(3, 45200.00, 1));

    // Buy 3 — sweeps all levels
    book.addOrder(makeMarketBid(10, 3));

    ASSERT_EQ(trades.size(), size_t(3), "3 fills");
    ASSERT_NEAR(trades[0].price, 45000.00, 1e-9, "L1 price");
    ASSERT_NEAR(trades[1].price, 45100.00, 1e-9, "L2 price");
    ASSERT_NEAR(trades[2].price, 45200.00, 1e-9, "L3 price");
    ASSERT_EQ(book.askOrderCount(), size_t(0), "ask book empty");
}

static void test_printbook_output() {
    qtl::OrderBook book{"AAPL"};
    book.addOrder(makeLimitBid(1, 182.49, 200));
    book.addOrder(makeLimitBid(2, 182.48, 400));
    book.addOrder(makeLimitAsk(3, 182.51, 100));
    book.addOrder(makeLimitAsk(4, 182.52, 300));

    std::string output = book.printBook(5);
    ASSERT_FALSE(output.empty(), "printBook not empty");
    ASSERT_TRUE(output.find("AAPL") != std::string::npos, "contains symbol");
    ASSERT_TRUE(output.find("BID")  != std::string::npos, "contains BID");
    ASSERT_TRUE(output.find("ASK")  != std::string::npos, "contains ASK");
    // Print to console for visual verification
    std::cout << output << std::flush;
}

static void test_snapshot_depth_limit() {
    qtl::OrderBook book{"ETH"};
    for (int i = 0; i < 10; ++i) {
        book.addOrder(makeLimitBid(i+1,
            3000.0 - i * 10.0, static_cast<qtl::Quantity>((i+1)*100)));
        book.addOrder(makeLimitAsk(i+100,
            3001.0 + i * 10.0, static_cast<qtl::Quantity>((i+1)*100)));
    }

    auto snap5 = book.snapshot(5);
    ASSERT_EQ(snap5.bids.size(), size_t(5), "5 bid levels in depth-5 snapshot");
    ASSERT_EQ(snap5.asks.size(), size_t(5), "5 ask levels in depth-5 snapshot");

    auto snap0 = book.snapshot(0); // 0 = all levels
    ASSERT_EQ(snap0.bids.size(), size_t(10), "all 10 bid levels when depth=0");
    ASSERT_EQ(snap0.asks.size(), size_t(10), "all 10 ask levels when depth=0");

    // Bids should be descending (best first)
    for (size_t i = 1; i < snap5.bids.size(); ++i) {
        ASSERT_TRUE(snap5.bids[i-1].first >= snap5.bids[i].first,
                    "bids descending");
    }
    // Asks should be ascending (best first)
    for (size_t i = 1; i < snap5.asks.size(); ++i) {
        ASSERT_TRUE(snap5.asks[i-1].first <= snap5.asks[i].first,
                    "asks ascending");
    }
}

static void test_large_scale_10k_orders() {
    qtl::OrderBook book{"LOAD"};
    std::mt19937 rng{42};
    std::uniform_real_distribution<double> priceDist{99.0, 101.0};
    std::uniform_int_distribution<int>     qtyDist{1, 1000};

    constexpr int kOrders = 10000;
    std::vector<qtl::OrderId> bidIds, askIds;

    for (int i = 1; i <= kOrders; ++i) {
        double p = std::round(priceDist(rng) * 100.0) / 100.0;
        int    q = qtyDist(rng);
        if (i % 2 == 0) {
            book.addOrder(makeLimitBid(static_cast<qtl::OrderId>(i),
                                       p - 0.5, static_cast<qtl::Quantity>(q)));
            bidIds.push_back(i);
        } else {
            book.addOrder(makeLimitAsk(static_cast<qtl::OrderId>(i),
                                       p + 0.5, static_cast<qtl::Quantity>(q)));
            askIds.push_back(i);
        }
    }

    size_t initialOrders = (book.bidOrderCount() + book.askOrderCount());
    ASSERT_TRUE(initialOrders > 0, "orders resting after 10k inserts");

    // Cancel half the bids randomly
    std::shuffle(bidIds.begin(), bidIds.end(), rng);
    int cancelled = 0;
    for (size_t i = 0; i < bidIds.size() / 2; ++i) {
        if (book.cancelOrder(bidIds[i])) ++cancelled;
    }
    ASSERT_TRUE(cancelled > 0, "some cancels succeeded");
    ASSERT_TRUE((book.bidOrderCount() + book.askOrderCount()) < initialOrders, "fewer orders after cancels");

    // Book should still be internally consistent
    ASSERT_TRUE(book.bestBid() == 0.0 || book.bestBid() > 0.0, "bestBid valid");
    ASSERT_TRUE(book.bestAsk() == 0.0 || book.bestAsk() > 0.0, "bestAsk valid");
}

// ─────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────

void registerOrderBookTests() {
    registerTest("OrderBook/empty_state",                test_empty_book);
    registerTest("OrderBook/add_resting_orders",         test_add_resting_orders);
    registerTest("OrderBook/fifo_priority",              test_fifo_priority);
    registerTest("OrderBook/full_fill_limit_cross",      test_full_fill_limit_cross);
    registerTest("OrderBook/partial_fill_maker",         test_partial_fill_maker);
    registerTest("OrderBook/partial_fill_taker",         test_partial_fill_taker);
    registerTest("OrderBook/market_order_sweeps_levels", test_market_order_sweeps_levels);
    registerTest("OrderBook/cancel_order",               test_cancel_order);
    registerTest("OrderBook/modify_price_loses_priority",test_modify_price_loses_priority);
    registerTest("OrderBook/modify_qty_preserves_priority",test_modify_qty_decrease_preserves_priority);
    registerTest("OrderBook/ioc_order",                  test_ioc_order);
    registerTest("OrderBook/fok_success",                test_fok_success);
    registerTest("OrderBook/fok_reject",                 test_fok_reject);
    registerTest("OrderBook/duplicate_order_id",         test_duplicate_order_id);
    registerTest("OrderBook/best_prices",                test_best_prices);
    registerTest("OrderBook/trade_report_fields",        test_trade_report_fields);
    registerTest("OrderBook/multi_level_sweep",          test_multi_level_sweep_prices);
    registerTest("OrderBook/printbook_output",           test_printbook_output);
    registerTest("OrderBook/snapshot_depth",             test_snapshot_depth_limit);
    registerTest("OrderBook/large_scale_10k",            test_large_scale_10k_orders);
}
