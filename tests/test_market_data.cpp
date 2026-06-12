/**
 * @file tests/test_market_data.cpp
 * @brief Phase 5 — Market Data Engine comprehensive tests.
 *
 * Tests cover:
 *  1.  TickParser: TradeTick CSV line
 *  2.  TickParser: QuoteTick CSV line
 *  3.  TickParser: OrderBookUpdate CSV line
 *  4.  TickParser: header detection (skip non-numeric first column)
 *  5.  TickParser: side parsing (B/S/BUY/SELL)
 *  6.  TickParser: action parsing (A/C/M/T/X)
 *  7.  TickParser: malformed line throws
 *  8.  AnyTick: tagged union construction and access
 *  9.  MarketDataFeed: trade tick processing
 *  10. MarketDataFeed: quote tick processing + MarketEvent emission
 *  11. MarketDataFeed: crossed quote detection
 *  12. MarketDataFeed: OHLCV bar aggregation over multiple trades
 *  13. MarketDataFeed: bar close on interval boundary
 *  14. MarketDataFeed: multi-symbol state isolation
 *  15. MarketDataFeed: FeedStats accumulation
 *  16. TickReplayer: loadCSV + replay (as-fast-as-possible)
 *  17. TickReplayer: pause + resume
 *  18. TickReplayer: multi-file merge (time-ordered)
 *  19. TickReplayer: SimClock driven by replay
 *  20. TickReplayer: tick callback fires for every tick
 *  21. TickReplayer: replay drives EventLoop MarketEvents
 *  22. TickReplayer: ReplayStats correctness
 *  23. TickReplayer: malformed lines skipped (parse errors counted)
 */

#include "tests/TestHelper.hpp"
#include "exchange/marketdata/Tick.hpp"
#include "exchange/marketdata/TickParser.hpp"
#include "exchange/marketdata/MarketDataFeed.hpp"
#include "backtesting/replay/TickReplayer.hpp"
#include "core/events/EventLoop.hpp"
#include "core/clock/Clock.hpp"

#include <functional>
#include <string>
#include <fstream>
#include <filesystem>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <sstream>
#include <iostream>

extern void registerTest(std::string, std::function<void()>);

// ─────────────────────────────────────────────────────────────
// CSV helpers — generate synthetic tick files
// ─────────────────────────────────────────────────────────────

static std::string makeTmpPath(const std::string& name) {
    return "/tmp/qtl_test_" + name + ".csv";
}

static void writeTradeCSV(const std::string& path, int nTicks,
                            const std::string& sym = "AAPL",
                            qtl::Timestamp startTs = 1700000000000000000LL,
                            int64_t intervalNs = 1'000'000LL) {
    std::ofstream f{path};
    f << "timestamp_ns,symbol,price,quantity,side,trade_id,exchange\n";
    for (int i = 0; i < nTicks; ++i) {
        qtl::Timestamp ts = startTs + static_cast<qtl::Timestamp>(i) * intervalNs;
        double price = 182.50 + (i % 10) * 0.01;
        int    qty   = 100 + (i % 5) * 50;
        char   side  = (i % 2 == 0) ? 'B' : 'S';
        f << ts << "," << sym << "," << std::fixed << price
          << "," << qty << "," << side
          << "," << (9000 + i) << ",NASDAQ\n";
    }
}

static void writeQuoteCSV(const std::string& path, int nTicks,
                            const std::string& sym = "AAPL",
                            qtl::Timestamp startTs = 1700000000000000000LL) {
    std::ofstream f{path};
    f << "timestamp_ns,symbol,bid_px,bid_sz,ask_px,ask_sz,exchange\n";
    for (int i = 0; i < nTicks; ++i) {
        qtl::Timestamp ts = startTs + static_cast<qtl::Timestamp>(i) * 500'000LL;
        double bid = 182.49 + (i % 5) * 0.01;
        double ask = bid + 0.02;
        f << ts << "," << sym << ","
          << std::fixed << bid << ",200,"
          << ask << ",150,NASDAQ\n";
    }
}

// ─────────────────────────────────────────────────────────────
// Tests
// ─────────────────────────────────────────────────────────────

static void test_parse_trade_tick() {
    std::string line = "1700000000000000000,AAPL,182.5000,100,B,9001,NASDAQ";
    auto t = qtl::TickParser::parseTrade(line);
    ASSERT_EQ(t.header.timestamp, qtl::Timestamp(1700000000000000000LL), "timestamp");
    ASSERT_EQ(t.header.symbol, "AAPL", "symbol");
    ASSERT_NEAR(t.price, 182.50, 1e-9, "price");
    ASSERT_EQ(t.quantity, qtl::Quantity(100), "quantity");
    ASSERT_TRUE(t.aggressorSide == qtl::Side::Buy, "side=Buy");
    ASSERT_EQ(t.tradeId, uint64_t(9001), "tradeId");
    ASSERT_EQ(t.header.exchange, "NASDAQ", "exchange");
}

static void test_parse_quote_tick() {
    std::string line = "1700000000000000000,MSFT,415.00,300,415.02,200,NASDAQ";
    auto q = qtl::TickParser::parseQuote(line);
    ASSERT_EQ(q.header.symbol, "MSFT", "symbol");
    ASSERT_NEAR(q.bidPrice, 415.00, 1e-9, "bid");
    ASSERT_EQ(q.bidSize, qtl::Quantity(300), "bidSz");
    ASSERT_NEAR(q.askPrice, 415.02, 1e-9, "ask");
    ASSERT_EQ(q.askSize, qtl::Quantity(200), "askSz");
    ASSERT_NEAR(q.spread(), 0.02, 1e-9, "spread");
    ASSERT_NEAR(q.midPrice(), 415.01, 1e-9, "mid");
    ASSERT_FALSE(q.isCrossed(), "not crossed");
}

static void test_parse_book_update() {
    std::string line = "1700000000000000000,SPY,A,B,450.00,500,12345,2";
    auto u = qtl::TickParser::parseBookUpdate(line);
    ASSERT_EQ(u.header.symbol, "SPY", "symbol");
    ASSERT_TRUE(u.action == qtl::BookAction::Add, "action=Add");
    ASSERT_TRUE(u.side == qtl::Side::Buy, "side=Buy");
    ASSERT_NEAR(u.price, 450.00, 1e-9, "price");
    ASSERT_EQ(u.quantity, qtl::Quantity(500), "qty");
    ASSERT_EQ(u.orderId, qtl::OrderId(12345), "orderId");
    ASSERT_EQ(u.level, 2, "level");
}

static void test_header_detection() {
    ASSERT_TRUE(qtl::TickParser::isHeaderOrComment(
        "timestamp_ns,symbol,price,quantity,side"), "header detected");
    ASSERT_TRUE(qtl::TickParser::isHeaderOrComment(""), "empty line = header");
    ASSERT_TRUE(qtl::TickParser::isHeaderOrComment("# comment"), "comment detected");
    ASSERT_FALSE(qtl::TickParser::isHeaderOrComment(
        "1700000000000000000,AAPL,182.50,100,B"), "data line = not header");
}

static void test_side_parsing() {
    ASSERT_TRUE(qtl::TickParser::parseSide("B")    == qtl::Side::Buy,  "B");
    ASSERT_TRUE(qtl::TickParser::parseSide("BUY")  == qtl::Side::Buy,  "BUY");
    ASSERT_TRUE(qtl::TickParser::parseSide("Buy")  == qtl::Side::Buy,  "Buy");
    ASSERT_TRUE(qtl::TickParser::parseSide("S")    == qtl::Side::Sell, "S");
    ASSERT_TRUE(qtl::TickParser::parseSide("SELL") == qtl::Side::Sell, "SELL");
    ASSERT_TRUE(qtl::TickParser::parseSide("1")    == qtl::Side::Buy,  "1");
    ASSERT_TRUE(qtl::TickParser::parseSide("0")    == qtl::Side::Sell, "0");
}

static void test_action_parsing() {
    ASSERT_TRUE(qtl::TickParser::parseAction("A")      == qtl::BookAction::Add,    "A");
    ASSERT_TRUE(qtl::TickParser::parseAction("ADD")    == qtl::BookAction::Add,    "ADD");
    ASSERT_TRUE(qtl::TickParser::parseAction("C")      == qtl::BookAction::Cancel, "C");
    ASSERT_TRUE(qtl::TickParser::parseAction("CANCEL") == qtl::BookAction::Cancel, "CANCEL");
    ASSERT_TRUE(qtl::TickParser::parseAction("M")      == qtl::BookAction::Modify, "M");
    ASSERT_TRUE(qtl::TickParser::parseAction("T")      == qtl::BookAction::Trade,  "T");
    ASSERT_TRUE(qtl::TickParser::parseAction("X")      == qtl::BookAction::Clear,  "X");
}

static void test_malformed_line_throws() {
    bool threw = false;
    try { qtl::TickParser::parseTrade("bad_line"); }
    catch (const std::exception&) { threw = true; }
    ASSERT_TRUE(threw, "malformed trade line throws");

    threw = false;
    try { qtl::TickParser::parseQuote("1700,AAPL"); }  // too few columns
    catch (const std::exception&) { threw = true; }
    ASSERT_TRUE(threw, "too few columns throws");
}

static void test_anytick_tagged_union() {
    // Trade
    qtl::TradeTick t;
    t.header.symbol = "AAPL"; t.price = 182.50; t.quantity = 100;
    auto any = qtl::AnyTick::makeTrade(t);
    ASSERT_TRUE(any.type == qtl::TickType::Trade, "type=Trade");
    ASSERT_EQ(any.symbol(), "AAPL", "symbol via symbol()");
    ASSERT_NEAR(any.trade.price, 182.50, 1e-9, "trade.price");

    // Quote
    qtl::QuoteTick q;
    q.header.symbol = "MSFT"; q.bidPrice = 415.0; q.askPrice = 415.02;
    auto anyQ = qtl::AnyTick::makeQuote(q);
    ASSERT_TRUE(anyQ.type == qtl::TickType::Quote, "type=Quote");
    ASSERT_NEAR(anyQ.quote.spread(), 0.02, 1e-9, "spread");

    // Copy constructor
    auto anyQ2 = anyQ;
    ASSERT_TRUE(anyQ2.type == qtl::TickType::Quote, "copy type");
    ASSERT_NEAR(anyQ2.quote.bidPrice, 415.0, 1e-9, "copy value");
}

static void test_feed_trade_processing() {
    qtl::MarketDataFeed feed;
    int tradeCount = 0;
    feed.onTrade([&](const qtl::TradeTick& t){
        ++tradeCount;
        ASSERT_NEAR(t.price, 182.50, 1e-9, "trade price");
    });

    qtl::TradeTick t;
    t.header.symbol = "AAPL";
    t.header.timestamp = 1700000000000000000LL;
    t.price = 182.50; t.quantity = 100;
    feed.processTick(qtl::AnyTick::makeTrade(t));

    ASSERT_EQ(tradeCount, 1, "trade callback fired");
    ASSERT_EQ(feed.stats().tradeTicks, uint64_t(1), "tradeTicks=1");
    ASSERT_EQ(feed.stats().totalTicks, uint64_t(1), "totalTicks=1");
}

static void test_feed_quote_and_marketevent() {
    qtl::EventLoop loop;
    int marketEvents = 0;
    loop.subscribe<qtl::MarketEvent>([&](const qtl::MarketEvent&){
        ++marketEvents;
    });
    std::thread loopThread([&]{ loop.run(qtl::RunMode::Run); });

    qtl::MarketDataFeed feed{&loop};
    int quoteCbCount = 0;
    feed.onQuote([&](const qtl::QuoteTick&){ ++quoteCbCount; });

    qtl::QuoteTick q;
    q.header.symbol = "AAPL"; q.header.timestamp = 1700000000000000000LL;
    q.bidPrice = 182.49; q.bidSize = 200;
    q.askPrice = 182.51; q.askSize = 150;
    feed.processTick(qtl::AnyTick::makeQuote(q));

    // Wait for EventLoop to process
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    while (marketEvents < 1 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds{5});

    loop.stop();
    loopThread.join();

    ASSERT_EQ(quoteCbCount, 1, "quote callback fired");
    ASSERT_TRUE(marketEvents >= 1, "MarketEvent emitted to loop");
    ASSERT_EQ(feed.stats().quoteTicks, uint64_t(1), "quoteTicks=1");
}

static void test_crossed_quote_detection() {
    qtl::MarketDataFeed feed;
    qtl::QuoteTick q;
    q.header.symbol = "TEST"; q.header.timestamp = 1700000000000000000LL;
    q.bidPrice = 100.05;  // bid > ask = crossed
    q.askPrice = 100.04;
    q.bidSize  = 100; q.askSize = 100;
    ASSERT_TRUE(q.isCrossed(), "crossed quote detected");

    feed.processTick(qtl::AnyTick::makeQuote(q));
    ASSERT_EQ(feed.stats().crossedQuotes, uint64_t(1), "crossedQuotes=1");
}

static void test_bar_aggregation() {
    qtl::MarketDataFeed feed;
    // 1-second bar interval
    feed.setBarInterval(1'000'000'000LL);

    std::vector<qtl::Bar> bars;
    feed.onBar([&](const qtl::Bar& b){ bars.push_back(b); });

    qtl::Timestamp base = 1700000000000000000LL;

    // 5 trades within first second
    for (int i = 0; i < 5; ++i) {
        qtl::TradeTick t;
        t.header.symbol    = "AAPL";
        t.header.timestamp = base + static_cast<qtl::Timestamp>(i) * 100'000'000LL; // 100ms apart
        t.price    = 182.50 + i * 0.01;
        t.quantity = 100;
        feed.processTick(qtl::AnyTick::makeTrade(t));
    }

    // Trade in second second — should close the first bar
    qtl::TradeTick closeT;
    closeT.header.symbol    = "AAPL";
    closeT.header.timestamp = base + 1'500'000'000LL; // +1.5s
    closeT.price    = 183.00;
    closeT.quantity = 50;
    feed.processTick(qtl::AnyTick::makeTrade(closeT));

    ASSERT_TRUE(bars.size() >= 1, "at least 1 bar closed");

    const qtl::Bar& bar = bars[0];
    // The bar is closed by the tick at +1.5s (183.00, qty=50).
    // That tick is included in the bar before close is detected.
    // So: 5 ticks @182.50-182.54 (100 each) + 1 tick @183.00 (50) = 550 volume.
    ASSERT_NEAR(bar.open,  182.50, 1e-9, "bar open");
    ASSERT_NEAR(bar.close, 183.00, 1e-9, "bar close (triggering tick included)");
    ASSERT_NEAR(bar.high,  183.00, 1e-9, "bar high");
    ASSERT_NEAR(bar.low,   182.50, 1e-9, "bar low");
    ASSERT_EQ(bar.volume, qtl::Quantity(550), "bar volume = 500 + 50");
    ASSERT_EQ(bar.tradeCount, uint64_t(6), "6 trades in bar (5 + trigger)");
    ASSERT_TRUE(bar.vwap() > 182.50 && bar.vwap() <= 183.00, "VWAP in range");
}

static void test_multi_symbol_isolation() {
    qtl::MarketDataFeed feed;
    int aaplCount = 0, msftCount = 0;
    feed.onTrade([&](const qtl::TradeTick& t){
        if (t.header.symbol == "AAPL") ++aaplCount;
        if (t.header.symbol == "MSFT") ++msftCount;
    });

    qtl::Timestamp ts = 1700000000000000000LL;
    for (int i = 0; i < 5; ++i) {
        qtl::TradeTick t;
        t.header.symbol = "AAPL"; t.header.timestamp = ts + i;
        t.price = 182.50; t.quantity = 100;
        feed.processTick(qtl::AnyTick::makeTrade(t));
    }
    for (int i = 0; i < 3; ++i) {
        qtl::TradeTick t;
        t.header.symbol = "MSFT"; t.header.timestamp = ts + i;
        t.price = 415.0; t.quantity = 200;
        feed.processTick(qtl::AnyTick::makeTrade(t));
    }

    ASSERT_EQ(aaplCount, 5, "5 AAPL trades");
    ASSERT_EQ(msftCount, 3, "3 MSFT trades");

    auto* aaplState = feed.getState("AAPL");
    auto* msftState = feed.getState("MSFT");
    ASSERT_TRUE(aaplState != nullptr, "AAPL state exists");
    ASSERT_TRUE(msftState != nullptr, "MSFT state exists");
    ASSERT_EQ(aaplState->tradeCount, uint64_t(5), "AAPL count=5");
    ASSERT_EQ(msftState->tradeCount, uint64_t(3), "MSFT count=3");
}

static void test_feed_stats() {
    qtl::MarketDataFeed feed;
    qtl::Timestamp ts = 1700000000000000000LL;

    for (int i = 0; i < 10; ++i) {
        qtl::TradeTick t;
        t.header.symbol = "X"; t.header.timestamp = ts + i * 1'000'000LL;
        t.price = 100.0; t.quantity = 1;
        feed.processTick(qtl::AnyTick::makeTrade(t));
    }
    for (int i = 0; i < 5; ++i) {
        qtl::QuoteTick q;
        q.header.symbol = "X"; q.header.timestamp = ts + i;
        q.bidPrice = 99.99; q.bidSize = 100;
        q.askPrice = 100.01; q.askSize = 100;
        feed.processTick(qtl::AnyTick::makeQuote(q));
    }

    ASSERT_EQ(feed.stats().totalTicks, uint64_t(15), "15 total");
    ASSERT_EQ(feed.stats().tradeTicks, uint64_t(10), "10 trades");
    ASSERT_EQ(feed.stats().quoteTicks, uint64_t(5), "5 quotes");
    ASSERT_TRUE(feed.stats().ticksPerSec() > 0, "tps > 0");
}

static void test_replayer_load_and_replay() {
    std::string path = makeTmpPath("trade1");
    writeTradeCSV(path, 1000);

    qtl::MarketDataFeed feed;
    std::atomic<int> tickCount{0};
    feed.onTrade([&](const qtl::TradeTick&){ ++tickCount; });

    qtl::TickReplayer replayer{&feed};
    replayer.loadCSV(path, qtl::TickType::Trade);
    replayer.replay(/*background=*/false);

    ASSERT_EQ(tickCount.load(), 1000, "all 1000 ticks replayed");
    ASSERT_EQ(replayer.stats().ticksReplayed, uint64_t(1000), "stats count=1000");
    ASSERT_TRUE(replayer.stats().dataStartTime > 0, "dataStartTime set");
    ASSERT_TRUE(replayer.stats().dataEndTime   > replayer.stats().dataStartTime,
                "dataEndTime > dataStartTime");
}

static void test_replayer_pause_resume() {
    std::string path = makeTmpPath("trade_pr");
    writeTradeCSV(path, 2000);

    qtl::MarketDataFeed feed;
    std::atomic<int> tickCount{0};
    std::atomic<bool> pauseSent{false};

    feed.onTrade([&](const qtl::TradeTick&){
        ++tickCount;
    });

    qtl::TickReplayer replayer{&feed};
    replayer.loadCSV(path, qtl::TickType::Trade);

    // Use tick callback to pause after 500 ticks
    replayer.setTickCallback([&](const qtl::AnyTick&){
        if (tickCount.load() == 500 && !pauseSent.exchange(true)) {
            replayer.pause();
        }
    });

    replayer.replay(/*background=*/true);

    // Wait for pause to take effect
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    while (!replayer.isPaused() &&
           std::chrono::steady_clock::now() < deadline &&
           replayer.isReplaying()) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    int countAtPause = tickCount.load();
    // Replay is paused — count should not advance
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    ASSERT_TRUE(tickCount.load() <= countAtPause + 5,
                "tick count stable while paused");

    // Resume
    replayer.resume();
    replayer.waitForCompletion();

    ASSERT_EQ(tickCount.load(), 2000, "all 2000 ticks after resume");
}

static void test_replayer_multi_file_merge() {
    // AAPL ticks: even timestamps
    std::string pathA = makeTmpPath("merge_aapl");
    writeTradeCSV(pathA, 500, "AAPL",
                  1700000000000000000LL, 2'000'000LL); // every 2ms

    // MSFT ticks: odd timestamps (interleaved)
    std::string pathM = makeTmpPath("merge_msft");
    writeTradeCSV(pathM, 500, "MSFT",
                  1700000001000000000LL, 2'000'000LL); // offset by 1ms

    qtl::MarketDataFeed feed;
    std::vector<qtl::Timestamp> timestamps;
    feed.onTrade([&](const qtl::TradeTick& t){
        timestamps.push_back(t.header.timestamp);
    });

    qtl::TickReplayer replayer{&feed};
    replayer.loadCSV(pathA, qtl::TickType::Trade);
    replayer.loadCSV(pathM, qtl::TickType::Trade);
    replayer.replay(false);

    ASSERT_EQ(timestamps.size(), size_t(1000), "all 1000 ticks (500+500)");

    // Verify timestamps are non-decreasing (merged in order)
    for (size_t i = 1; i < timestamps.size(); ++i) {
        ASSERT_TRUE(timestamps[i] >= timestamps[i-1],
                    "timestamps non-decreasing after merge");
    }
}

static void test_replayer_drives_simclock() {
    std::string path = makeTmpPath("clock");
    qtl::Timestamp startTs = 1700000000000000000LL;
    writeTradeCSV(path, 100, "AAPL", startTs, 1'000'000LL);

    qtl::SimClock clock{0};
    qtl::MarketDataFeed feed;
    qtl::TickReplayer replayer{&feed, &clock, 0.0};
    replayer.loadCSV(path, qtl::TickType::Trade);
    replayer.replay(false);

    // After replay, SimClock should equal last tick timestamp
    ASSERT_TRUE(clock.now() >= startTs, "clock advanced to at least startTs");
    ASSERT_TRUE(clock.now() <= startTs + 100 * 1'000'000LL + 1'000'000LL,
                "clock in expected range");
}

static void test_replayer_tick_callback() {
    std::string path = makeTmpPath("callback");
    writeQuoteCSV(path, 200);

    qtl::MarketDataFeed feed;
    std::atomic<int> cbCount{0};

    qtl::TickReplayer replayer{&feed};
    replayer.loadCSV(path, qtl::TickType::Quote);
    replayer.setTickCallback([&](const qtl::AnyTick&){ ++cbCount; });
    replayer.replay(false);

    ASSERT_EQ(cbCount.load(), 200, "tick callback fired 200 times");
}

static void test_replayer_drives_eventloop() {
    std::string path = makeTmpPath("evloop");
    writeQuoteCSV(path, 50);

    qtl::EventLoop loop;
    std::atomic<int> marketEvents{0};
    loop.subscribe<qtl::MarketEvent>([&](const qtl::MarketEvent&){
        ++marketEvents;
    });
    std::thread loopThread([&]{ loop.run(qtl::RunMode::Run); });

    qtl::MarketDataFeed feed{&loop};
    qtl::TickReplayer replayer{&feed};
    replayer.loadCSV(path, qtl::TickType::Quote);
    replayer.replay(false); // synchronous

    // Wait for EventLoop to drain
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (marketEvents.load() < 50 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    loop.stop();
    loopThread.join();

    ASSERT_EQ(marketEvents.load(), 50, "50 MarketEvents from 50 quote ticks");
}

static void test_replayer_stats() {
    std::string path = makeTmpPath("stats");
    writeTradeCSV(path, 300);

    qtl::MarketDataFeed feed;
    qtl::TickReplayer replayer{&feed};
    replayer.loadCSV(path, qtl::TickType::Trade);
    replayer.replay(false);

    const auto& s = replayer.stats();
    ASSERT_EQ(s.ticksReplayed, uint64_t(300), "300 replayed");
    ASSERT_TRUE(s.wallDurationSec() >= 0, "wall duration >= 0");
    ASSERT_TRUE(s.dataDurationSec() > 0,  "data duration > 0");
    ASSERT_TRUE(s.actualSpeedFactor() > 0,"speed factor > 0");
}

static void test_replayer_malformed_lines_skipped() {
    std::string path = makeTmpPath("malformed");
    {
        std::ofstream f{path};
        f << "timestamp_ns,symbol,price,quantity,side,trade_id,exchange\n";
        f << "1700000000000000000,AAPL,182.50,100,B,9001,NASDAQ\n";
        f << "BAD_LINE_NOT_PARSEABLE\n";  // malformed — should be skipped
        f << "1700000000000000001,AAPL,182.51,200,S,9002,NASDAQ\n";
        f << "\n";                          // empty line — skipped
        f << "# comment line\n";            // comment — skipped
        f << "1700000000000000002,AAPL,182.52,300,B,9003,NASDAQ\n";
    }

    qtl::MarketDataFeed feed;
    int goodTicks = 0;
    feed.onTrade([&](const qtl::TradeTick&){ ++goodTicks; });

    qtl::TickReplayer replayer{&feed};
    replayer.loadCSV(path, qtl::TickType::Trade);
    replayer.replay(false);

    // 3 valid lines parsed, 1 malformed skipped
    ASSERT_EQ(goodTicks, 3, "3 valid ticks processed");
}

// ─────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────

void registerMarketDataTests() {
    registerTest("MarketData/parse_trade_tick",           test_parse_trade_tick);
    registerTest("MarketData/parse_quote_tick",           test_parse_quote_tick);
    registerTest("MarketData/parse_book_update",          test_parse_book_update);
    registerTest("MarketData/header_detection",           test_header_detection);
    registerTest("MarketData/side_parsing",               test_side_parsing);
    registerTest("MarketData/action_parsing",             test_action_parsing);
    registerTest("MarketData/malformed_line_throws",      test_malformed_line_throws);
    registerTest("MarketData/anytick_union",              test_anytick_tagged_union);
    registerTest("MarketData/feed_trade_processing",      test_feed_trade_processing);
    registerTest("MarketData/feed_quote_and_event",       test_feed_quote_and_marketevent);
    registerTest("MarketData/crossed_quote",              test_crossed_quote_detection);
    registerTest("MarketData/bar_aggregation",            test_bar_aggregation);
    registerTest("MarketData/multi_symbol_isolation",     test_multi_symbol_isolation);
    registerTest("MarketData/feed_stats",                 test_feed_stats);
    registerTest("MarketData/replayer_load_replay",       test_replayer_load_and_replay);
    registerTest("MarketData/replayer_pause_resume",      test_replayer_pause_resume);
    registerTest("MarketData/replayer_multi_file_merge",  test_replayer_multi_file_merge);
    registerTest("MarketData/replayer_drives_simclock",   test_replayer_drives_simclock);
    registerTest("MarketData/replayer_tick_callback",     test_replayer_tick_callback);
    registerTest("MarketData/replayer_drives_eventloop",  test_replayer_drives_eventloop);
    registerTest("MarketData/replayer_stats",             test_replayer_stats);
    registerTest("MarketData/replayer_malformed_skipped", test_replayer_malformed_lines_skipped);
}
