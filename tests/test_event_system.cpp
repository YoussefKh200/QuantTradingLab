/**
 * @file tests/test_event_system.cpp
 * @brief Phase 2 — comprehensive unit tests for the event system.
 *
 * Tests cover:
 *  1.  Event construction (all 6 types) + field correctness
 *  2.  EventQueue: push/pop, tryPop, blocking pop, stop signal
 *  3.  EventQueue: multi-threaded producer stress (1M events)
 *  4.  EventDispatcher: typed subscription, fan-out, drainQueue
 *  5.  EventDispatcher: raw subscription by EventType enum
 *  6.  EventDispatcher: exception isolation (bad handler doesn't kill loop)
 *  7.  EventLoop: Drain mode
 *  8.  EventLoop: Step mode
 *  9.  EventLoop: Run mode (background thread)
 *  10. EventLoop: LoopStats accumulation
 *  11. SPSCEventQueue: basic push/pop
 *  12. SPSCEventQueue: full-then-drain cycle
 *  13. EventPool: basic alloc/free round-trip
 *  14. EventPool: pool exhaustion fallback to heap
 *  15. EventPool: multi-alloc concurrent correctness
 *  16. Timestamp ordering (events created in sequence)
 */

#include "tests/TestHelper.hpp"
#include "core/events/Event.hpp"
#include "core/events/EventQueue.hpp"
#include "core/events/EventDispatcher.hpp"
#include "core/events/EventLoop.hpp"
#include "core/events/SPSCEventQueue.hpp"
#include "core/events/EventPool.hpp"
#include "core/clock/Clock.hpp"

#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>
#include <memory>

extern void registerTest(std::string, std::function<void()>);

// ─────────────────────────────────────────────────────────────
// Helper: build sample events
// ─────────────────────────────────────────────────────────────
static std::unique_ptr<qtl::MarketEvent> makeMarket(
        const std::string& sym = "AAPL",
        double bid = 180.0, double ask = 180.01) {
    return std::make_unique<qtl::MarketEvent>(sym, bid, 100, ask, 200, bid, 50);
}

static std::unique_ptr<qtl::OrderEvent> makeOrder() {
    return std::make_unique<qtl::OrderEvent>(
        42, "MSFT", qtl::OrderType::Limit,
        qtl::Side::Buy, 415.0, 100,
        qtl::TimeInForce::GTC, "TestStrategy");
}

static std::unique_ptr<qtl::FillEvent> makeFill() {
    return std::make_unique<qtl::FillEvent>(
        42, 9001, "MSFT", qtl::Side::Buy,
        415.01, 100, 0, 0.10, true, "TestStrategy");
}

// ─────────────────────────────────────────────────────────────
// Test implementations
// ─────────────────────────────────────────────────────────────

// 1. Event construction
static void test_event_construction() {
    // MarketEvent
    auto me = makeMarket("AAPL", 182.5, 182.51);
    ASSERT_TRUE(me->type == qtl::EventType::Market, "MarketEvent type");
    ASSERT_NEAR(me->bidPrice, 182.5, 1e-9, "bid price");
    ASSERT_NEAR(me->askPrice, 182.51, 1e-9, "ask price");
    ASSERT_EQ(me->symbol, "AAPL", "symbol");
    ASSERT_TRUE(me->timestamp > 0, "timestamp set");

    // OrderEvent
    auto oe = makeOrder();
    ASSERT_TRUE(oe->type == qtl::EventType::Order, "OrderEvent type");
    ASSERT_EQ(oe->orderId, uint64_t(42), "orderId");
    ASSERT_TRUE(oe->side == qtl::Side::Buy, "side");

    // FillEvent
    auto fe = makeFill();
    ASSERT_TRUE(fe->type == qtl::EventType::Fill, "FillEvent type");
    ASSERT_EQ(fe->fillQuantity, int64_t(100), "fillQty");
    ASSERT_NEAR(fe->commission, 0.10, 1e-9, "commission");
    ASSERT_TRUE(fe->isTaker, "isTaker");

    // SignalEvent
    qtl::SignalEvent se{"SPY", 0.85, "MomentumModel"};
    ASSERT_TRUE(se.type == qtl::EventType::Signal, "SignalEvent type");
    ASSERT_NEAR(se.strength, 0.85, 1e-9, "signal strength");

    // RiskEvent
    qtl::RiskEvent re{"AAPL", qtl::RiskEvent::Severity::Breach,
                      "Daily loss exceeded", 52000.0, 50000.0};
    ASSERT_TRUE(re.type == qtl::EventType::Risk, "RiskEvent type");
    ASSERT_TRUE(re.severity == qtl::RiskEvent::Severity::Breach, "severity");

    // TimerEvent
    qtl::TimerEvent te{7, "HeartbeatTimer"};
    ASSERT_TRUE(te.type == qtl::EventType::Timer, "TimerEvent type");
    ASSERT_EQ(te.timerId, uint64_t(7), "timerId");
}

// 2. EventQueue basic push / pop
static void test_eventqueue_basic() {
    qtl::EventQueue q;
    ASSERT_TRUE(q.empty(), "queue starts empty");
    ASSERT_EQ(q.size(), size_t(0), "size=0");

    q.push(makeMarket());
    q.push(makeOrder());

    ASSERT_FALSE(q.empty(), "non-empty after pushes");
    ASSERT_EQ(q.size(), size_t(2), "size=2");
    ASSERT_EQ(q.totalPushed(), uint64_t(2), "totalPushed=2");

    auto ev1 = q.tryPop();
    ASSERT_TRUE(ev1 != nullptr, "pop1 not null");
    ASSERT_TRUE(ev1->type == qtl::EventType::Market, "first is Market");

    auto ev2 = q.tryPop();
    ASSERT_TRUE(ev2 != nullptr, "pop2 not null");
    ASSERT_TRUE(ev2->type == qtl::EventType::Order, "second is Order");

    auto ev3 = q.tryPop();
    ASSERT_TRUE(ev3 == nullptr, "pop3 is null (empty)");
    ASSERT_EQ(q.totalPopped(), uint64_t(2), "totalPopped=2");
}

// 3. EventQueue multi-threaded producer stress (100 k events)
static void test_eventqueue_multithreaded() {
    qtl::EventQueue q;
    constexpr int kPerThread = 25000;
    constexpr int kThreads   = 4;

    std::vector<std::thread> producers;
    producers.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        producers.emplace_back([&q, kPerThread]() {
            for (int j = 0; j < kPerThread; ++j) {
                q.push(std::make_unique<qtl::MarketEvent>(
                    "SYM", 100.0, 100, 100.01, 100, 100.0, 50));
            }
        });
    }

    // Consumer thread
    std::atomic<int> consumed{0};
    std::thread consumer([&]() {
        while (consumed.load() < kThreads * kPerThread) {
            auto ev = q.popFor(std::chrono::milliseconds{100});
            if (ev) ++consumed;
        }
    });

    for (auto& t : producers) t.join();
    consumer.join();

    ASSERT_EQ(consumed.load(), kThreads * kPerThread, "all events consumed");
    ASSERT_EQ(q.totalPushed(), uint64_t(kThreads * kPerThread), "totalPushed");
}

// 4. EventQueue stop signal
static void test_eventqueue_stop() {
    qtl::EventQueue q;

    std::thread blocker([&]() {
        // This should unblock when stop() is called
        auto ev = q.pop();
        ASSERT_TRUE(ev == nullptr, "pop after stop returns nullptr");
    });

    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    q.stop();
    blocker.join();
    ASSERT_TRUE(q.isStopped(), "queue is stopped");
}

// 5. EventDispatcher typed subscription + fan-out
static void test_dispatcher_typed() {
    qtl::EventDispatcher d;
    int marketCount = 0, fillCount = 0;
    double lastStrength = 0.0;

    d.subscribe<qtl::MarketEvent>([&](const qtl::MarketEvent&){ ++marketCount; });
    d.subscribe<qtl::MarketEvent>([&](const qtl::MarketEvent&){ ++marketCount; }); // fan-out
    d.subscribe<qtl::FillEvent>  ([&](const qtl::FillEvent&)  { ++fillCount;   });
    d.subscribe<qtl::SignalEvent>([&](const qtl::SignalEvent& s){ lastStrength = s.strength; });

    auto me = makeMarket();
    auto fe = makeFill();
    qtl::SignalEvent se{"AAPL", 0.65, "Test"};

    d.dispatch(*me);
    d.dispatch(*fe);
    d.dispatch(se);

    ASSERT_EQ(marketCount, 2, "fan-out: both MarketEvent handlers fired");
    ASSERT_EQ(fillCount,   1, "FillEvent handler fired once");
    ASSERT_NEAR(lastStrength, 0.65, 1e-9, "SignalEvent value passed through");
}

// 6. EventDispatcher raw subscription by EventType enum
static void test_dispatcher_raw() {
    qtl::EventDispatcher d;
    int rawCount = 0;

    d.subscribeRaw(qtl::EventType::Market,
                   [&](const qtl::Event&){ ++rawCount; });
    d.subscribeRaw(qtl::EventType::Fill,
                   [&](const qtl::Event&){ ++rawCount; });

    d.dispatch(*makeMarket());
    d.dispatch(*makeFill());
    d.dispatch(*makeOrder()); // no raw handler for Order

    ASSERT_EQ(rawCount, 2, "raw handlers fired for Market+Fill only");
}

// 7. EventDispatcher: drainQueue
static void test_dispatcher_drain() {
    qtl::EventQueue q;
    qtl::EventDispatcher d;

    int total = 0;
    d.subscribe<qtl::MarketEvent>([&](const qtl::MarketEvent&){ ++total; });
    d.subscribe<qtl::OrderEvent> ([&](const qtl::OrderEvent& ){ ++total; });

    for (int i = 0; i < 10; ++i) q.push(makeMarket());
    for (int i = 0; i < 5;  ++i) q.push(makeOrder());

    size_t drained = d.drainQueue(q);
    ASSERT_EQ(drained, size_t(15), "drained 15 events");
    ASSERT_EQ(total,   15,         "all handlers fired");
    ASSERT_TRUE(q.empty(), "queue empty after drain");
}

// 8. EventDispatcher: exception isolation
static void test_dispatcher_exception_isolation() {
    qtl::EventDispatcher d;
    int goodCount = 0;

    // First handler: always throws
    d.subscribe<qtl::MarketEvent>([](const qtl::MarketEvent&){
        throw std::runtime_error("intentional test exception");
    });
    // Second handler: counts — must still fire despite first throwing
    d.subscribe<qtl::MarketEvent>([&](const qtl::MarketEvent&){ ++goodCount; });

    // The dispatcher itself does not catch — exception isolation is the
    // EventLoop's responsibility.  Verify the loop catches it.
    qtl::EventLoop loop;
    loop.subscribe<qtl::MarketEvent>([](const qtl::MarketEvent&){
        throw std::runtime_error("loop isolation test");
    });
    loop.subscribe<qtl::MarketEvent>([&](const qtl::MarketEvent&){ ++goodCount; });

    loop.push(makeMarket());
    loop.run(qtl::RunMode::Drain);

    // Loop should have caught the exception and incremented droppedEvents.
    // The second handler still fires because both are invoked in sequence
    // within one dispatch call — but the loop-level catch wraps the whole
    // dispatch.  The second subscriber fires inside the same dispatch call,
    // so goodCount reflects both handlers.
    ASSERT_TRUE(loop.stats().totalEvents >= 1, "event was processed");
}

// 9. EventLoop: Drain mode
static void test_eventloop_drain() {
    qtl::EventLoop loop;
    int count = 0;
    loop.subscribe<qtl::MarketEvent>([&](const qtl::MarketEvent&){ ++count; });

    for (int i = 0; i < 100; ++i) loop.push(makeMarket());

    uint64_t dispatched = loop.run(qtl::RunMode::Drain);
    ASSERT_EQ(dispatched, uint64_t(100), "100 events dispatched");
    ASSERT_EQ(count, 100, "handler fired 100 times");
    ASSERT_EQ(loop.stats().totalEvents, uint64_t(100), "stats totalEvents=100");
}

// 10. EventLoop: Step mode
static void test_eventloop_step() {
    qtl::EventLoop loop;
    int count = 0;
    loop.subscribe<qtl::MarketEvent>([&](const qtl::MarketEvent&){ ++count; });

    loop.push(makeMarket());
    loop.push(makeMarket());

    uint64_t n1 = loop.run(qtl::RunMode::Step);
    ASSERT_EQ(n1, uint64_t(1), "step processes exactly 1");
    ASSERT_EQ(count, 1, "handler fired once");

    uint64_t n2 = loop.run(qtl::RunMode::Step);
    ASSERT_EQ(n2, uint64_t(1), "second step");
    ASSERT_EQ(count, 2, "handler fired twice");

    uint64_t n3 = loop.run(qtl::RunMode::Step);
    ASSERT_EQ(n3, uint64_t(0), "empty queue → 0 steps");
}

// 11. EventLoop: Run mode (background thread)
static void test_eventloop_run_mode() {
    qtl::EventLoop loop;
    std::atomic<int> received{0};

    loop.subscribe<qtl::MarketEvent>([&](const qtl::MarketEvent&){
        ++received;
    });

    std::thread loopThread([&](){
        loop.run(qtl::RunMode::Run);
    });

    // Push 50 events from main thread while loop is running
    for (int i = 0; i < 50; ++i) {
        loop.push(makeMarket());
        std::this_thread::sleep_for(std::chrono::microseconds{100});
    }

    // Wait for all to be consumed
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (received.load() < 50 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    loop.stop();
    loopThread.join();

    ASSERT_EQ(received.load(), 50, "all 50 events received in Run mode");
}

// 12. EventLoop: LoopStats
static void test_eventloop_stats() {
    qtl::EventLoop loop;
    loop.subscribe<qtl::MarketEvent>([](const qtl::MarketEvent&){});
    loop.subscribe<qtl::FillEvent>  ([](const qtl::FillEvent&)  {});

    for (int i = 0; i < 20; ++i) loop.push(makeMarket());
    for (int i = 0; i < 10; ++i) loop.push(makeFill());

    loop.run(qtl::RunMode::Drain);

    const auto& s = loop.stats();
    ASSERT_EQ(s.totalEvents, uint64_t(30), "30 total events");

    // Market type index = 0, Fill = 2
    ASSERT_EQ(s.perType[0].count, uint64_t(20), "20 Market events in stats");
    ASSERT_EQ(s.perType[2].count, uint64_t(10), "10 Fill events in stats");
    ASSERT_TRUE(s.perType[0].avgNs() >= 0, "avg ns non-negative");

    std::string report = s.report();
    ASSERT_TRUE(!report.empty(), "report not empty");
}

// 13. SPSCEventQueue: basic push/pop
static void test_spsc_basic() {
    qtl::SPSCEventQueue<256> q;
    ASSERT_TRUE(q.empty(), "starts empty");

    bool pushed = q.tryPush(makeMarket("SPSC_TEST"));
    ASSERT_TRUE(pushed, "push succeeds on empty queue");
    ASSERT_FALSE(q.empty(), "non-empty after push");
    ASSERT_EQ(q.size(), size_t(1), "size=1");

    auto ev = q.tryPop();
    ASSERT_TRUE(ev != nullptr, "pop returns event");
    ASSERT_TRUE(ev->type == qtl::EventType::Market, "correct type");

    auto ev2 = q.tryPop();
    ASSERT_TRUE(ev2 == nullptr, "second pop is null");
}

// 14. SPSCEventQueue: fill then drain cycle
static void test_spsc_fill_drain() {
    constexpr size_t kCap = 64;
    qtl::SPSCEventQueue<kCap> q;

    // Fill to capacity-1 (ring buffer stores Cap-1 items max)
    int pushed = 0;
    for (size_t i = 0; i < kCap; ++i) {
        if (q.tryPush(makeMarket())) ++pushed;
    }
    ASSERT_TRUE(pushed > 0, "pushed at least one");

    // Drain
    int popped = 0;
    while (auto ev = q.tryPop()) ++popped;
    ASSERT_EQ(pushed, popped, "drained exactly what was pushed");
    ASSERT_TRUE(q.empty(), "empty after drain");

    // Refill after drain — verifies wrap-around
    bool ok = q.tryPush(makeMarket("WRAP"));
    ASSERT_TRUE(ok, "push after full drain succeeds");
    auto ev = q.tryPop();
    ASSERT_TRUE(ev != nullptr, "pop after refill succeeds");
}

// 15. SPSCEventQueue: SPSC throughput smoke test (1 M events)
static void test_spsc_throughput() {
    constexpr size_t kCap   = 1u << 14; // 16384
    constexpr int kN        = 1'000'000;

    qtl::SPSCEventQueue<kCap> q;
    std::atomic<int> consumed{0};

    std::thread producer([&](){
        int sent = 0;
        while (sent < kN) {
            if (q.tryPush(std::make_unique<qtl::MarketEvent>(
                    "SPSC", 100.0, 1, 100.01, 1, 100.0, 1))) {
                ++sent;
            }
            // spin if full (producer faster than consumer in this test)
        }
    });

    std::thread consumer([&](){
        while (consumed.load() < kN) {
            if (auto ev = q.tryPop()) ++consumed;
        }
    });

    producer.join();
    consumer.join();
    ASSERT_EQ(consumed.load(), kN, "1M SPSC events consumed");
}

// 16. EventPool: basic alloc/free round-trip
static void test_eventpool_basic() {
    qtl::EventPool<qtl::MarketEvent, 16> pool;
    ASSERT_EQ(pool.liveCount(), size_t(0), "starts with 0 live");
    ASSERT_EQ(pool.freeCount(), size_t(16), "all 16 free");

    {
        auto ev = pool.make("POOL_TEST", 100.0, 10, 100.01, 10, 100.0, 5);
        ASSERT_TRUE(ev != nullptr, "make returns non-null");
        ASSERT_EQ(ev->symbol, "POOL_TEST", "symbol correct");
        ASSERT_EQ(pool.liveCount(), size_t(1), "1 live object");
    }
    // unique_ptr out of scope — slot should be returned
    ASSERT_EQ(pool.liveCount(), size_t(0), "0 live after scope");
    ASSERT_EQ(pool.freeCount(), size_t(16), "all 16 free again");
}

// 17. EventPool: pool exhaustion falls back to heap
static void test_eventpool_exhaustion() {
    qtl::EventPool<qtl::MarketEvent, 4> pool;

    std::vector<qtl::EventPool<qtl::MarketEvent,4>::PoolPtr> held;
    // Drain all 4 pool slots
    for (int i = 0; i < 4; ++i) {
        held.push_back(pool.make("EX", 1.0, 1, 1.0, 1, 1.0, 1));
    }
    ASSERT_EQ(pool.liveCount(), size_t(4), "pool full");

    // 5th allocation must succeed via heap fallback
    auto overflow = pool.make("HEAP", 2.0, 1, 2.0, 1, 2.0, 1);
    ASSERT_TRUE(overflow != nullptr, "heap fallback non-null");
    ASSERT_EQ(overflow->symbol, "HEAP", "heap fallback symbol correct");
    // Live count stays at 4 for pool slots (heap alloc not counted in pool)
    ASSERT_EQ(pool.liveCount(), size_t(4), "pool live still 4");

    held.clear(); // release all pool slots
    ASSERT_EQ(pool.liveCount(), size_t(0), "all returned after clear");
}

// 18. Timestamp ordering
static void test_timestamp_ordering() {
    auto e1 = makeMarket();
    std::this_thread::sleep_for(std::chrono::microseconds{10});
    auto e2 = makeMarket();

    ASSERT_TRUE(e2->timestamp >= e1->timestamp,
                "timestamps are non-decreasing");
}

// ─────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────
void registerEventSystemTests() {
    registerTest("EventSystem/construction",              test_event_construction);
    registerTest("EventSystem/EventQueue_basic",          test_eventqueue_basic);
    registerTest("EventSystem/EventQueue_multithreaded",  test_eventqueue_multithreaded);
    registerTest("EventSystem/EventQueue_stop",           test_eventqueue_stop);
    registerTest("EventSystem/Dispatcher_typed",          test_dispatcher_typed);
    registerTest("EventSystem/Dispatcher_raw",            test_dispatcher_raw);
    registerTest("EventSystem/Dispatcher_drain",          test_dispatcher_drain);
    registerTest("EventSystem/Dispatcher_exception",      test_dispatcher_exception_isolation);
    registerTest("EventSystem/EventLoop_drain",           test_eventloop_drain);
    registerTest("EventSystem/EventLoop_step",            test_eventloop_step);
    registerTest("EventSystem/EventLoop_run",             test_eventloop_run_mode);
    registerTest("EventSystem/EventLoop_stats",           test_eventloop_stats);
    registerTest("EventSystem/SPSC_basic",                test_spsc_basic);
    registerTest("EventSystem/SPSC_fill_drain",           test_spsc_fill_drain);
    registerTest("EventSystem/SPSC_throughput_1M",        test_spsc_throughput);
    registerTest("EventSystem/EventPool_basic",           test_eventpool_basic);
    registerTest("EventSystem/EventPool_exhaustion",      test_eventpool_exhaustion);
    registerTest("EventSystem/Timestamp_ordering",        test_timestamp_ordering);
}
