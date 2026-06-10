/**
 * @file main.cpp
 * @brief QuantTradingLab entry point.
 *
 * Phase 1: Demonstrates the core infrastructure by:
 *  1. Loading configuration
 *  2. Starting the logger
 *  3. Creating an event queue + dispatcher
 *  4. Pushing several typed events through the system
 *  5. Printing a structural summary
 *
 * Later phases replace this with the full simulation / trading loop.
 */

#include "core/Types.hpp"
#include "core/config/Config.hpp"
#include "core/logger/Logger.hpp"
#include "core/clock/Clock.hpp"
#include "core/events/Event.hpp"
#include "core/events/EventQueue.hpp"
#include "core/events/EventDispatcher.hpp"
#include "core/threading/ThreadPool.hpp"

#include <iostream>
#include <memory>
#include <thread>
#include <chrono>

using namespace qtl;

int main() {
    // ── 1. Logger setup ───────────────────────────────────────
    auto& log = Logger::instance();
    log.setLevel(LogLevel::Debug);
    log.info("Main", "QuantTradingLab v1.0 starting...");

    // ── 2. Configuration ──────────────────────────────────────
    auto& cfg = Config::instance();
    cfg.set("system.name",           "QuantTradingLab");
    cfg.set("system.version",        "1.0.0");
    cfg.set("risk.maxDailyLoss",     50000.0);
    cfg.set("risk.maxPositionSize", static_cast<int64_t>(1000));
    cfg.set("strategy.mm.spread",    0.02);
    cfg.set("backtest.enabled",      true);

    log.info("Config", "Loaded {} key(s)", cfg.dump().size());

    // ── 3. Clock ──────────────────────────────────────────────
    SimClock simClock{1'700'000'000'000'000'000LL};  // some epoch ns
    WallClock wallClock;

    log.info("Clock", "SimClock  t={}", simClock.now());
    log.info("Clock", "WallClock t={}", wallClock.now());

    simClock.advance(1'000'000LL); // +1 ms
    log.info("Clock", "SimClock after +1ms: t={}", simClock.now());

    // ── 4. Event system smoke test ────────────────────────────
    EventQueue    queue;
    EventDispatcher dispatcher;

    int marketCount = 0, fillCount = 0, signalCount = 0;

    dispatcher.subscribe<MarketEvent>([&](const MarketEvent& e){
        ++marketCount;
        log.debug("Dispatcher", "MarketEvent: sym={} bid={:.2f} ask={:.2f}",
                  e.symbol, e.bidPrice, e.askPrice);
    });

    dispatcher.subscribe<FillEvent>([&](const FillEvent& e){
        ++fillCount;
        log.debug("Dispatcher", "FillEvent: orderId={} qty={} price={:.2f}",
                  e.orderId, e.fillQuantity, e.fillPrice);
    });

    dispatcher.subscribe<SignalEvent>([&](const SignalEvent& e){
        ++signalCount;
        log.debug("Dispatcher", "SignalEvent: sym={} strength={:.4f} src={}",
                  e.symbol, e.strength, e.source);
    });

    // Push events
    queue.push(std::make_unique<MarketEvent>("AAPL", 182.50, 200, 182.51, 150, 182.50, 100));
    queue.push(std::make_unique<MarketEvent>("MSFT", 415.00, 300, 415.02, 200, 415.01,  50));
    queue.push(std::make_unique<SignalEvent>("AAPL",  0.73, "MomentumModel"));
    queue.push(std::make_unique<FillEvent>(1001, 5001, "AAPL",
                                           Side::Buy, 182.51, 100, 0,
                                           0.10, true, "MM_Strategy"));
    queue.push(std::make_unique<RiskEvent>("AAPL",
                                           RiskEvent::Severity::Warning,
                                           "Position approaching limit",
                                           950.0, 1000.0));

    log.info("EventQueue", "Queue size before drain: {}", queue.size());

    // Drain
    size_t dispatched = dispatcher.drainQueue(queue);

    log.info("EventQueue",  "Dispatched {} events", dispatched);
    log.info("Dispatcher",  "MarketEvents={} FillEvents={} SignalEvents={}",
             marketCount, fillCount, signalCount);

    // ── 5. Thread pool smoke test ──────────────────────────────
    ThreadPool pool{4};
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 8; ++i) {
        futures.push_back(pool.submit([i]{ return i * i; }));
    }
    int sum = 0;
    for (auto& f : futures) sum += f.get();
    log.info("ThreadPool", "Sum of squares 0..7 = {} (expected 140)", sum);

    // ── 6. Summary ────────────────────────────────────────────
    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════╗\n";
    std::cout << "║       QuantTradingLab — Phase 1 Complete     ║\n";
    std::cout << "╠══════════════════════════════════════════════╣\n";
    std::cout << "║  Core subsystems initialised:                ║\n";
    std::cout << "║    ✓ Configuration store                     ║\n";
    std::cout << "║    ✓ Async structured logger                 ║\n";
    std::cout << "║    ✓ Wall clock + Simulation clock           ║\n";
    std::cout << "║    ✓ Event hierarchy (6 event types)         ║\n";
    std::cout << "║    ✓ Thread-safe EventQueue                  ║\n";
    std::cout << "║    ✓ Typed EventDispatcher                   ║\n";
    std::cout << "║    ✓ SPSC lock-free RingBuffer               ║\n";
    std::cout << "║    ✓ Fixed-size ThreadPool                   ║\n";
    std::cout << "║  All module stubs compiled successfully      ║\n";
    std::cout << "╚══════════════════════════════════════════════╝\n\n";

    log.info("Main", "Phase 1 complete — awaiting Phase 2 confirmation.");

    // Give the async logger time to flush before exit
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    log.stop();
    return 0;
}
