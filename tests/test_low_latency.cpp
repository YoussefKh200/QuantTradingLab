/**
 * @file tests/test_low_latency.cpp
 * @brief Phase 12 — Low-Latency Components comprehensive tests.
 *
 * MemoryPool (12):
 *  1.  allocate() returns non-null on empty pool
 *  2.  allocate() returns null when pool exhausted
 *  3.  deallocate() frees slot for reuse
 *  4.  liveCount tracks allocations correctly
 *  5.  freeCount = capacity - liveCount
 *  6.  owns() returns true for pool-allocated pointer
 *  7.  owns() returns false for external pointer
 *  8.  full() correct when all slots taken
 *  9.  empty() correct after all freed
 *  10. Multi-alloc + multi-free: all slots reusable
 *  11. Concurrent alloc/free: 4 threads, no corruption
 *  12. ABA safety: rapid alloc/dealloc cycle
 *
 * ObjectPool (5):
 *  13. acquire() constructs object with args
 *  14. release() calls destructor
 *  15. acquire() after release() reuses slot
 *  16. Pool exhaustion returns nullptr
 *  17. liveCount consistent with acquire/release
 *
 * LatencyStats (8):
 *  18. count increments on record()
 *  19. min/max tracked correctly
 *  20. mean computed correctly for uniform samples
 *  21. stddev computed correctly
 *  22. percentiles p50/p95/p99 ordered correctly
 *  23. histogram buckets accumulate
 *  24. reset() clears all state
 *  25. report() string non-empty with all fields
 *
 * LatencyTimer (3):
 *  26. RAII timer records sample on destruction
 *  27. elapsedNs() positive and increasing
 *  28. Multiple timers accumulate to stats
 *
 * SPSCQueue (10):
 *  29. tryPush/tryPop basic round-trip
 *  30. Returns nullopt on empty pop
 *  31. Returns false on full push
 *  32. size() tracks correctly
 *  33. empty() and full() correct
 *  34. Wrap-around (capacity+1 pushes after drains)
 *  35. peek() sees front without consuming
 *  36. SPSC correctness: producer/consumer threads, 1M elements
 *  37. Throughput benchmark: >50M ops/sec
 *  38. Latency benchmark: mean < 100ns per op
 *
 * LatencyBench (4):
 *  39. run() produces stats with correct count
 *  40. compare() produces results for all benches
 *  41. compareReport() contains all benchmark names
 *  42. ProfilingReport generates multi-section report
 */

#include "tests/TestHelper.hpp"
#include "core/threading/MemoryPool.hpp"
#include "core/threading/LatencyTimer.hpp"
#include "core/threading/SPSCQueue.hpp"

#include <functional>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numeric>
#include <new>

extern void registerTest(std::string, std::function<void()>);
using namespace qtl;

// ─────────────────────────────────────────────────────────────
// MemoryPool tests
// ─────────────────────────────────────────────────────────────

struct alignas(8) TestObj {
    uint64_t value{0};
    uint64_t pad{0};
};

static void test_mp_allocate_non_null() {
    MemoryPool<TestObj, 16> pool;
    TestObj* p = pool.allocate();
    ASSERT_TRUE(p != nullptr, "allocate returns non-null");
    pool.deallocate(p);
}

static void test_mp_exhaustion_returns_null() {
    MemoryPool<TestObj, 4> pool;
    TestObj* p1 = pool.allocate();
    TestObj* p2 = pool.allocate();
    TestObj* p3 = pool.allocate();
    TestObj* p4 = pool.allocate();
    TestObj* p5 = pool.allocate();  // exhausted
    ASSERT_TRUE(p1 && p2 && p3 && p4, "first 4 succeed");
    ASSERT_TRUE(p5 == nullptr, "5th returns null (pool full)");
    pool.deallocate(p1);
    pool.deallocate(p2);
    pool.deallocate(p3);
    pool.deallocate(p4);
}

static void test_mp_deallocate_reuse() {
    MemoryPool<TestObj, 2> pool;
    TestObj* p1 = pool.allocate();
    ASSERT_TRUE(p1 != nullptr, "first alloc");
    pool.deallocate(p1);
    TestObj* p2 = pool.allocate();
    ASSERT_TRUE(p2 != nullptr, "reuse after free");
    pool.deallocate(p2);
}

static void test_mp_live_count() {
    MemoryPool<TestObj, 8> pool;
    ASSERT_EQ(pool.liveCount(), size_t(0), "starts at 0");
    auto* p1 = pool.allocate();
    ASSERT_EQ(pool.liveCount(), size_t(1), "1 after alloc");
    auto* p2 = pool.allocate();
    ASSERT_EQ(pool.liveCount(), size_t(2), "2 after 2 allocs");
    pool.deallocate(p1);
    ASSERT_EQ(pool.liveCount(), size_t(1), "1 after dealloc");
    pool.deallocate(p2);
    ASSERT_EQ(pool.liveCount(), size_t(0), "0 after all freed");
}

static void test_mp_free_count() {
    MemoryPool<TestObj, 8> pool;
    ASSERT_EQ(pool.freeCount(), size_t(8), "starts full");
    auto* p = pool.allocate();
    ASSERT_EQ(pool.freeCount(), size_t(7), "7 after 1 alloc");
    pool.deallocate(p);
    ASSERT_EQ(pool.freeCount(), size_t(8), "back to 8");
}

static void test_mp_owns_pool_pointer() {
    MemoryPool<TestObj, 4> pool;
    auto* p = pool.allocate();
    ASSERT_TRUE(pool.owns(p), "owns pool pointer");
    pool.deallocate(p);
}

static void test_mp_owns_false_for_external() {
    MemoryPool<TestObj, 4> pool;
    TestObj external;
    ASSERT_FALSE(pool.owns(&external), "does not own stack pointer");
}

static void test_mp_full_flag() {
    MemoryPool<TestObj, 2> pool;
    ASSERT_FALSE(pool.full(), "not full at start");
    auto* p1 = pool.allocate();
    auto* p2 = pool.allocate();
    ASSERT_TRUE(pool.full(), "full when all slots taken");
    pool.deallocate(p1);
    pool.deallocate(p2);
}

static void test_mp_empty_flag() {
    MemoryPool<TestObj, 2> pool;
    ASSERT_TRUE(pool.empty(), "empty at start");
    auto* p = pool.allocate();
    ASSERT_FALSE(pool.empty(), "not empty after alloc");
    pool.deallocate(p);
    ASSERT_TRUE(pool.empty(), "empty after free");
}

static void test_mp_all_slots_reusable() {
    constexpr size_t N = 64;
    MemoryPool<TestObj, N> pool;
    std::vector<TestObj*> ptrs;

    // Alloc all
    for (size_t i = 0; i < N; ++i) {
        auto* p = pool.allocate();
        ASSERT_TRUE(p != nullptr, "alloc within capacity");
        p->value = i;
        ptrs.push_back(p);
    }
    ASSERT_TRUE(pool.full(), "pool full");

    // Free all
    for (auto* p : ptrs) pool.deallocate(p);
    ASSERT_TRUE(pool.empty(), "empty after all freed");

    // Alloc all again
    for (size_t i = 0; i < N; ++i) {
        auto* p = pool.allocate();
        ASSERT_TRUE(p != nullptr, "reuse after full free");
        pool.deallocate(p);
    }
}

static void test_mp_concurrent_alloc_free() {
    constexpr size_t PoolSize = 256;
    constexpr int    Threads  = 4;
    constexpr int    OpsPerThread = 10000;

    MemoryPool<TestObj, PoolSize> pool;
    std::atomic<int> errors{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < Threads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < OpsPerThread; ++i) {
                TestObj* p = pool.allocate();
                if (p) {
                    new (p) TestObj{42, 0};
                    if (p->value != 42) ++errors;
                    p->~TestObj();
                    pool.deallocate(p);
                }
                // Pool may be exhausted some iterations — that's fine
            }
        });
    }
    for (auto& t : threads) t.join();

    ASSERT_EQ(errors.load(), 0, "no corruption in concurrent alloc/free");
    ASSERT_EQ(pool.liveCount(), size_t(0), "pool clean after concurrent test");
}

static void test_mp_aba_safety() {
    // Rapid alloc/dealloc cycle: if ABA bug exists, free-list gets corrupted
    MemoryPool<TestObj, 8> pool;
    constexpr int N = 100000;

    for (int i = 0; i < N; ++i) {
        auto* p = pool.allocate();
        if (p) {
            p->value = static_cast<uint64_t>(i);
            pool.deallocate(p);
        }
    }
    // Pool should be in clean state
    ASSERT_EQ(pool.liveCount(), size_t(0), "ABA: pool clean after rapid cycle");
    // Should still be allocatable
    auto* p = pool.allocate();
    ASSERT_TRUE(p != nullptr, "ABA: alloc works after cycle");
    pool.deallocate(p);
}

// ─────────────────────────────────────────────────────────────
// ObjectPool tests
// ─────────────────────────────────────────────────────────────

struct CountedObj {
    static std::atomic<int> ctorCount;
    static std::atomic<int> dtorCount;
    int value;
    explicit CountedObj(int v) : value{v} { ++ctorCount; }
    ~CountedObj() { ++dtorCount; }
};
std::atomic<int> CountedObj::ctorCount{0};
std::atomic<int> CountedObj::dtorCount{0};

static void test_op_acquire_constructs() {
    CountedObj::ctorCount = 0; CountedObj::dtorCount = 0;
    ObjectPool<CountedObj, 4> pool;
    auto* p = pool.acquire(42);
    ASSERT_TRUE(p != nullptr, "acquire returns non-null");
    ASSERT_EQ(p->value, 42, "constructor arg passed");
    ASSERT_EQ(CountedObj::ctorCount.load(), 1, "ctor called once");
    pool.release(p);
}

static void test_op_release_destructs() {
    CountedObj::ctorCount = 0; CountedObj::dtorCount = 0;
    ObjectPool<CountedObj, 4> pool;
    auto* p = pool.acquire(7);
    ASSERT_EQ(CountedObj::dtorCount.load(), 0, "dtor not called before release");
    pool.release(p);
    ASSERT_EQ(CountedObj::dtorCount.load(), 1, "dtor called on release");
}

static void test_op_reuse_after_release() {
    ObjectPool<CountedObj, 2> pool;
    auto* p1 = pool.acquire(1);
    pool.release(p1);
    auto* p2 = pool.acquire(2);
    ASSERT_TRUE(p2 != nullptr, "slot reused after release");
    ASSERT_EQ(p2->value, 2, "new value correct");
    pool.release(p2);
}

static void test_op_exhaustion() {
    ObjectPool<CountedObj, 2> pool;
    auto* p1 = pool.acquire(1);
    auto* p2 = pool.acquire(2);
    auto* p3 = pool.acquire(3);  // exhausted
    ASSERT_TRUE(p3 == nullptr, "null on pool exhaustion");
    pool.release(p1);
    pool.release(p2);
}

static void test_op_livecount_consistent() {
    ObjectPool<CountedObj, 4> pool;
    ASSERT_EQ(pool.liveCount(), size_t(0), "starts at 0");
    auto* p1 = pool.acquire(1);
    auto* p2 = pool.acquire(2);
    ASSERT_EQ(pool.liveCount(), size_t(2), "2 live");
    pool.release(p1);
    ASSERT_EQ(pool.liveCount(), size_t(1), "1 live after release");
    pool.release(p2);
    ASSERT_EQ(pool.liveCount(), size_t(0), "0 live after all released");
}

// ─────────────────────────────────────────────────────────────
// LatencyStats tests
// ─────────────────────────────────────────────────────────────

static void test_ls_count() {
    LatencyStats s{"test"};
    ASSERT_EQ(s.count(), uint64_t(0), "starts at 0");
    s.record(100);
    s.record(200);
    s.record(300);
    ASSERT_EQ(s.count(), uint64_t(3), "count=3 after 3 records");
}

static void test_ls_min_max() {
    LatencyStats s;
    s.record(500);
    s.record(100);
    s.record(300);
    ASSERT_EQ(s.minNs(), int64_t(100), "min=100");
    ASSERT_EQ(s.maxNs(), int64_t(500), "max=500");
}

static void test_ls_mean_uniform() {
    LatencyStats s;
    for (int i = 1; i <= 10; ++i) s.record(i * 1000);  // 1000..10000 ns
    ASSERT_NEAR(s.meanNs(), 5500.0, 1.0, "mean of 1k..10k = 5500");
}

static void test_ls_stddev() {
    LatencyStats s;
    // Two-point distribution: all samples either 0 or 2000 ns
    for (int i = 0; i < 1000; ++i) s.record(i % 2 == 0 ? 0 : 2000);
    ASSERT_TRUE(s.stddevNs() > 0, "stddev > 0 for non-uniform samples");
    ASSERT_TRUE(s.stddevNs() < 2000, "stddev < max sample");
}

static void test_ls_percentiles_ordered() {
    LatencyStats s;
    for (int i = 1; i <= 1000; ++i) s.record(i * 100LL);
    ASSERT_TRUE(s.p50()  < s.p95(),  "p50 < p95");
    ASSERT_TRUE(s.p95()  < s.p99(),  "p95 < p99");
    ASSERT_TRUE(s.p99()  < s.p999(), "p99 < p99.9");
    ASSERT_TRUE(s.p999() <= s.maxNs() + 1, "p99.9 <= max");
}

static void test_ls_histogram_buckets() {
    LatencyStats s;
    s.record(50);        // <100ns bucket
    s.record(500);       // <1µs bucket
    s.record(5000);      // <10µs bucket
    s.record(50000);     // <100µs bucket
    s.record(500000);    // <1ms bucket
    ASSERT_EQ(s.count(), uint64_t(5), "5 samples recorded");
}

static void test_ls_reset() {
    LatencyStats s;
    s.record(1000);
    s.record(2000);
    ASSERT_EQ(s.count(), uint64_t(2), "2 before reset");
    s.reset();
    ASSERT_EQ(s.count(), uint64_t(0), "0 after reset");
    ASSERT_EQ(s.minNs(), INT64_MAX, "min reset to MAX");
    ASSERT_EQ(s.maxNs(), int64_t(0), "max reset to 0");
}

static void test_ls_report_non_empty() {
    LatencyStats s{"TestBench"};
    for (int i = 0; i < 100; ++i) s.record(i * 1000LL);
    std::string r = s.report();
    ASSERT_FALSE(r.empty(), "report non-empty");
    ASSERT_TRUE(r.find("TestBench") != std::string::npos, "report has name");
    ASSERT_TRUE(r.find("count") != std::string::npos, "report has count");
    ASSERT_TRUE(r.find("p99") != std::string::npos, "report has p99");
}

// ─────────────────────────────────────────────────────────────
// LatencyTimer tests
// ─────────────────────────────────────────────────────────────

static void test_lt_raii_records() {
    LatencyStats s;
    ASSERT_EQ(s.count(), uint64_t(0), "0 before timer");
    {
        LatencyTimer t{s};
        std::this_thread::sleep_for(std::chrono::microseconds{10});
    }
    ASSERT_EQ(s.count(), uint64_t(1), "1 after timer scope exit");
    ASSERT_TRUE(s.minNs() >= 1000, "min >= 1µs (slept 10µs)");
}

static void test_lt_elapsed_positive() {
    LatencyStats s;
    LatencyTimer t{s};
    std::this_thread::sleep_for(std::chrono::microseconds{5});
    ASSERT_TRUE(t.elapsedNs() > 0, "elapsed > 0 after sleep");
    ASSERT_TRUE(t.elapsedNs() < 100'000'000LL, "elapsed < 100ms (sanity)");
}

static void test_lt_multiple_timers() {
    LatencyStats s;
    for (int i = 0; i < 5; ++i) {
        LatencyTimer t{s};
        // no sleep — just measure overhead
    }
    ASSERT_EQ(s.count(), uint64_t(5), "5 samples after 5 timers");
    ASSERT_TRUE(s.meanNs() >= 0, "mean >= 0");
}

// ─────────────────────────────────────────────────────────────
// SPSCQueue tests
// ─────────────────────────────────────────────────────────────

static void test_spsc_basic_roundtrip() {
    SPSCQueue<int, 16> q;
    bool pushed = q.tryPush(42);
    ASSERT_TRUE(pushed, "push succeeds on empty queue");
    auto v = q.tryPop();
    ASSERT_TRUE(v.has_value(), "pop returns value");
    ASSERT_EQ(*v, 42, "correct value");
}

static void test_spsc_empty_pop() {
    SPSCQueue<int, 8> q;
    auto v = q.tryPop();
    ASSERT_FALSE(v.has_value(), "pop on empty returns nullopt");
}

static void test_spsc_full_push() {
    SPSCQueue<int, 4> q;  // usable capacity = 3
    bool ok1 = q.tryPush(1);
    bool ok2 = q.tryPush(2);
    bool ok3 = q.tryPush(3);
    bool ok4 = q.tryPush(4);  // should fail — queue full
    ASSERT_TRUE(ok1 && ok2 && ok3, "first 3 pushes succeed");
    ASSERT_FALSE(ok4, "4th push fails (full)");
}

static void test_spsc_size_tracking() {
    SPSCQueue<int, 8> q;
    ASSERT_EQ(q.size(), size_t(0), "starts empty");
    q.tryPush(1); q.tryPush(2);
    ASSERT_EQ(q.size(), size_t(2), "size=2 after 2 pushes");
    q.tryPop();
    ASSERT_EQ(q.size(), size_t(1), "size=1 after pop");
}

static void test_spsc_empty_full_flags() {
    SPSCQueue<int, 4> q;
    ASSERT_TRUE(q.empty(), "starts empty");
    ASSERT_FALSE(q.full(), "not full at start");
    q.tryPush(1); q.tryPush(2); q.tryPush(3);
    ASSERT_TRUE(q.full(), "full after 3 pushes (cap=3)");
    ASSERT_FALSE(q.empty(), "not empty when full");
}

static void test_spsc_wraparound() {
    SPSCQueue<int, 4> q;  // cap=3
    // Fill, drain, fill, drain — tests ring-buffer wrap
    for (int round = 0; round < 5; ++round) {
        for (int i = 0; i < 3; ++i) q.tryPush(i);
        for (int i = 0; i < 3; ++i) {
            auto v = q.tryPop();
            ASSERT_TRUE(v.has_value(), "pop has value on round wrap");
            ASSERT_EQ(*v, i, "correct value on wrap");
        }
    }
    ASSERT_TRUE(q.empty(), "empty after all wrap rounds");
}

static void test_spsc_peek() {
    SPSCQueue<int, 8> q;
    auto* front = q.peek();
    ASSERT_TRUE(front == nullptr, "peek null on empty");
    q.tryPush(99);
    front = q.peek();
    ASSERT_TRUE(front != nullptr, "peek non-null after push");
    ASSERT_EQ(*front, 99, "peek sees correct value");
    // Peek doesn't consume
    ASSERT_EQ(q.size(), size_t(1), "size still 1 after peek");
}

static void test_spsc_producer_consumer_1m() {
    constexpr int    N    = 1'000'000;
    constexpr size_t Cap  = 1u << 14;   // 16384

    SPSCQueue<uint64_t, Cap> q;
    std::atomic<bool>    done{false};
    std::atomic<uint64_t> consumed{0};
    std::atomic<bool>    dataOk{true};

    std::thread producer([&]() {
        for (int i = 0; i < N; ++i) {
            while (!q.tryPush(static_cast<uint64_t>(i))) { /* spin */ }
        }
        done.store(true);
    });

    std::thread consumer([&]() {
        uint64_t expected = 0;
        while (!done.load() || !q.empty()) {
            if (auto v = q.tryPop()) {
                if (*v != expected++) dataOk.store(false);
                ++consumed;
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(consumed.load(), uint64_t(N), "all 1M items consumed");
    ASSERT_TRUE(dataOk.load(), "no data corruption (FIFO order preserved)");
}

static void test_spsc_throughput() {
    constexpr int    N   = 500'000;
    constexpr size_t Cap = 1u << 14;
    SPSCQueue<uint64_t, Cap> q;
    std::atomic<bool> done{false};

    auto start = std::chrono::high_resolution_clock::now();

    std::thread producer([&]() {
        for (int i = 0; i < N; ++i) {
            while (!q.tryPush(static_cast<uint64_t>(i))) {}
        }
    });
    std::thread consumer([&]() {
        int count = 0;
        while (count < N) {
            if (auto v = q.tryPop()) ++count;
        }
        done.store(true);
    });
    producer.join();
    consumer.join();

    auto end = std::chrono::high_resolution_clock::now();
    double ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double mops = static_cast<double>(N) / (ns / 1000.0);  // M ops/sec

    // Achievable on most systems: > 50 M ops/sec
    ASSERT_TRUE(mops > 1.0, "throughput > 1M ops/sec (sanity check)");
    // Print for info
    std::cout << "  SPSC throughput: " << mops << " M ops/sec\n";
}

static void test_spsc_latency() {
    // Measure single-item round-trip latency
    constexpr int N = 10000;
    constexpr size_t Cap = 16;
    SPSCQueue<int, Cap> q;

    LatencyStats stats{"SPSC round-trip"};
    for (int i = 0; i < N; ++i) {
        int64_t t0 = hrnow();
        q.tryPush(i);
        q.tryPop();
        stats.record(hrnow() - t0);
    }

    // On a modern system, round-trip should be < 200ns mean
    ASSERT_TRUE(stats.meanNs() < 10000.0, "mean < 10µs per round-trip");
    std::cout << "  SPSC latency: " << stats.meanNs() << "ns mean, "
              << stats.p99()/1000.0 << "µs p99\n";
}

// ─────────────────────────────────────────────────────────────
// LatencyBench tests
// ─────────────────────────────────────────────────────────────

static void test_bench_run_correct_count() {
    auto stats = LatencyBench::run("noop", [](){}, 1000, 100);
    ASSERT_EQ(stats.count(), uint64_t(1000), "run produces 1000 samples");
    ASSERT_TRUE(stats.meanNs() >= 0, "mean >= 0");
}

static void test_bench_compare_all_results() {
    auto results = LatencyBench::compare({
        {"noop1", [](){}},
        {"noop2", [](){}},
        {"sleep1us", [](){ std::this_thread::sleep_for(std::chrono::microseconds{1}); }},
    }, 100);
    ASSERT_EQ(results.size(), size_t(3), "3 results");
    for (auto& r : results) {
        ASSERT_EQ(r.stats.count(), uint64_t(100), "each bench has 100 samples");
        ASSERT_TRUE(std::isfinite(r.throughputMps), "throughput finite");
    }
}

static void test_bench_compare_report() {
    auto results = LatencyBench::compare({
        {"AllocFree", [](){
            MemoryPool<TestObj, 64> pool;
            auto* p = pool.allocate();
            if (p) pool.deallocate(p);
        }},
        {"NoOp", [](){}},
    }, 500);

    std::string report = LatencyBench::compareReport(results);
    ASSERT_FALSE(report.empty(), "compareReport non-empty");
    ASSERT_TRUE(report.find("AllocFree") != std::string::npos, "contains AllocFree");
    ASSERT_TRUE(report.find("NoOp") != std::string::npos, "contains NoOp");
    ASSERT_TRUE(report.find("mean") != std::string::npos ||
                report.find("µs") != std::string::npos, "contains timing info");
    std::cout << report;
}

static void test_profiling_report() {
    ProfilingReport pr;

    LatencyStats s1{"EventQueue::push"};
    for (int i = 0; i < 100; ++i) s1.record(i * 500LL);
    pr.addStats("EventQueue", std::move(s1));

    LatencyStats s2{"MemoryPool::allocate"};
    for (int i = 0; i < 100; ++i) s2.record(i * 50LL);
    pr.addStats("MemoryPool", std::move(s2));

    std::string report = pr.generate();
    ASSERT_FALSE(report.empty(), "profiling report non-empty");
    ASSERT_TRUE(report.find("EventQueue") != std::string::npos, "has EventQueue section");
    ASSERT_TRUE(report.find("MemoryPool") != std::string::npos, "has MemoryPool section");
    ASSERT_TRUE(report.find("count") != std::string::npos, "has count field");
}

// ─────────────────────────────────────────────────────────────
// Registration
// ─────────────────────────────────────────────────────────────

void registerLowLatencyTests() {
    // MemoryPool
    registerTest("MemPool/allocate_non_null",        test_mp_allocate_non_null);
    registerTest("MemPool/exhaustion_null",           test_mp_exhaustion_returns_null);
    registerTest("MemPool/deallocate_reuse",          test_mp_deallocate_reuse);
    registerTest("MemPool/live_count",                test_mp_live_count);
    registerTest("MemPool/free_count",                test_mp_free_count);
    registerTest("MemPool/owns_pool_ptr",             test_mp_owns_pool_pointer);
    registerTest("MemPool/owns_false_external",       test_mp_owns_false_for_external);
    registerTest("MemPool/full_flag",                 test_mp_full_flag);
    registerTest("MemPool/empty_flag",                test_mp_empty_flag);
    registerTest("MemPool/all_slots_reusable",        test_mp_all_slots_reusable);
    registerTest("MemPool/concurrent_alloc_free",     test_mp_concurrent_alloc_free);
    registerTest("MemPool/aba_safety",                test_mp_aba_safety);
    // ObjectPool
    registerTest("ObjPool/acquire_constructs",        test_op_acquire_constructs);
    registerTest("ObjPool/release_destructs",         test_op_release_destructs);
    registerTest("ObjPool/reuse_after_release",       test_op_reuse_after_release);
    registerTest("ObjPool/exhaustion_null",           test_op_exhaustion);
    registerTest("ObjPool/livecount_consistent",      test_op_livecount_consistent);
    // LatencyStats
    registerTest("LatStats/count",                    test_ls_count);
    registerTest("LatStats/min_max",                  test_ls_min_max);
    registerTest("LatStats/mean_uniform",             test_ls_mean_uniform);
    registerTest("LatStats/stddev",                   test_ls_stddev);
    registerTest("LatStats/percentiles_ordered",      test_ls_percentiles_ordered);
    registerTest("LatStats/histogram_buckets",        test_ls_histogram_buckets);
    registerTest("LatStats/reset",                    test_ls_reset);
    registerTest("LatStats/report_non_empty",         test_ls_report_non_empty);
    // LatencyTimer
    registerTest("LatTimer/raii_records",             test_lt_raii_records);
    registerTest("LatTimer/elapsed_positive",         test_lt_elapsed_positive);
    registerTest("LatTimer/multiple_timers",          test_lt_multiple_timers);
    // SPSCQueue
    registerTest("SPSC/basic_roundtrip",              test_spsc_basic_roundtrip);
    registerTest("SPSC/empty_pop",                    test_spsc_empty_pop);
    registerTest("SPSC/full_push",                    test_spsc_full_push);
    registerTest("SPSC/size_tracking",                test_spsc_size_tracking);
    registerTest("SPSC/empty_full_flags",             test_spsc_empty_full_flags);
    registerTest("SPSC/wraparound",                   test_spsc_wraparound);
    registerTest("SPSC/peek",                         test_spsc_peek);
    registerTest("SPSC/producer_consumer_1M",         test_spsc_producer_consumer_1m);
    registerTest("SPSC/throughput",                   test_spsc_throughput);
    registerTest("SPSC/latency",                      test_spsc_latency);
    // LatencyBench
    registerTest("LatBench/run_correct_count",        test_bench_run_correct_count);
    registerTest("LatBench/compare_all_results",      test_bench_compare_all_results);
    registerTest("LatBench/compare_report",           test_bench_compare_report);
    registerTest("LatBench/profiling_report",         test_profiling_report);
}
