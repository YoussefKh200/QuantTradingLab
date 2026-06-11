#pragma once
/**
 * @file core/events/EventLoop.hpp
 * @brief Production-grade event loop for the trading system.
 *
 * Architecture
 * ────────────
 * The EventLoop is the central nervous system.  It owns an EventQueue,
 * an EventDispatcher, and a SimClock.  It drives the canonical pattern:
 *
 *   while (queue.hasEvents())
 *       dispatcher.dispatch(*queue.pop());
 *
 * Extended with:
 *  • Configurable run modes: DRAIN (process until empty), RUN (block until
 *    stopped), STEP (process one event).
 *  • Pre/post-dispatch hooks for latency profiling.
 *  • Per-event-type latency histogram (nanosecond resolution).
 *  • Thread-safe: producers push from any thread; the loop runs on one
 *    dedicated thread (or the calling thread for backtests).
 *  • Graceful shutdown: stop() drains remaining events then exits.
 *
 * Typical backtest usage:
 * @code
 *   EventLoop loop;
 *   loop.dispatcher().subscribe<MarketEvent>([&](const MarketEvent& e){ … });
 *   loop.dispatcher().subscribe<FillEvent>  ([&](const FillEvent&   e){ … });
 *
 *   // Feed events (from replay engine)
 *   loop.push(make_heap_event<MarketEvent>(…));
 *   loop.push(make_heap_event<OrderEvent>(…));
 *
 *   loop.run(RunMode::Drain);   // process all, return
 *   auto& stats = loop.stats(); // inspect latency
 * @endcode
 *
 * Typical live / paper-trading usage:
 * @code
 *   EventLoop loop;
 *   // … register handlers …
 *   std::thread t([&]{ loop.run(RunMode::Run); });
 *   // producers push from market-data thread, order-router thread, …
 *   loop.stop();  // signals the loop to drain + exit
 *   t.join();
 * @endcode
 */

#include "core/events/Event.hpp"
#include "core/events/EventQueue.hpp"
#include "core/events/EventDispatcher.hpp"
#include "core/clock/Clock.hpp"
#include "core/logger/Logger.hpp"

#include <atomic>
#include <functional>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <memory>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// Per-EventType latency accumulator
// ─────────────────────────────────────────────────────────────

struct EventTypeStats {
    uint64_t count{0};
    uint64_t totalNs{0};
    uint64_t minNs{UINT64_MAX};
    uint64_t maxNs{0};

    void record(uint64_t ns) noexcept {
        ++count;
        totalNs += ns;
        if (ns < minNs) minNs = ns;
        if (ns > maxNs) maxNs = ns;
    }

    [[nodiscard]] double avgNs() const noexcept {
        return count ? static_cast<double>(totalNs) / count : 0.0;
    }

    void reset() noexcept {
        count = 0; totalNs = 0;
        minNs = UINT64_MAX; maxNs = 0;
    }
};

// ─────────────────────────────────────────────────────────────
// LoopStats — aggregate counters for one run
// ─────────────────────────────────────────────────────────────

struct LoopStats {
    uint64_t totalEvents{0};
    uint64_t droppedEvents{0};      ///< handler threw an exception
    uint64_t totalDispatchNs{0};    ///< wall time inside dispatch calls
    uint64_t loopIterations{0};

    // Per-type breakdown (indexed by EventType enum value)
    static constexpr size_t kNumTypes = 8;
    std::array<EventTypeStats, kNumTypes> perType{};

    void recordDispatch(EventType t, uint64_t ns) noexcept {
        ++totalEvents;
        totalDispatchNs += ns;
        auto idx = static_cast<size_t>(t);
        if (idx < kNumTypes) perType[idx].record(ns);
    }

    void reset() noexcept {
        totalEvents = 0; droppedEvents = 0;
        totalDispatchNs = 0; loopIterations = 0;
        for (auto& s : perType) s.reset();
    }

    [[nodiscard]] std::string report() const;
};

// ─────────────────────────────────────────────────────────────
// RunMode
// ─────────────────────────────────────────────────────────────

enum class RunMode : uint8_t {
    /// Process one event and return (useful for unit tests)
    Step,
    /// Process all events currently in the queue, then return
    Drain,
    /// Block until stop() is called, processing events as they arrive
    Run
};

// ─────────────────────────────────────────────────────────────
// EventLoop
// ─────────────────────────────────────────────────────────────

class EventLoop {
public:
    /// Hook called before every dispatch: (event, loop_iteration_count)
    using PreDispatchHook  = std::function<void(const Event&, uint64_t)>;
    /// Hook called after every dispatch: (event, dispatch_ns)
    using PostDispatchHook = std::function<void(const Event&, uint64_t)>;

    explicit EventLoop(std::shared_ptr<IClock> clock = nullptr)
        : clock_{clock ? std::move(clock) : std::make_shared<WallClock>()}
    {}

    // Non-copyable
    EventLoop(const EventLoop&)            = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    // ── Producer API ─────────────────────────────────────────────

    /// Push any unique_ptr<Event> (heap or pool) onto the queue.
    void push(std::unique_ptr<Event> ev) {
        queue_.push(std::move(ev));
    }

    /// Template helper — constructs directly on heap and pushes.
    template<typename T, typename... Args>
    void emplace(Args&&... args) {
        queue_.push(std::make_unique<T>(std::forward<Args>(args)...));
    }

    // ── Handler registration (delegates to dispatcher) ───────────

    template<typename T>
    void subscribe(std::function<void(const T&)> handler) {
        dispatcher_.subscribe<T>(std::move(handler));
    }

    void subscribeRaw(EventType type, EventDispatcher::Handler handler) {
        dispatcher_.subscribeRaw(type, std::move(handler));
    }

    // ── Hooks ────────────────────────────────────────────────────

    void setPreDispatchHook(PreDispatchHook h)  { preHook_  = std::move(h); }
    void setPostDispatchHook(PostDispatchHook h) { postHook_ = std::move(h); }

    // ── Run control ──────────────────────────────────────────────

    /**
     * @brief Execute the event loop.
     *
     * @param mode  Step / Drain / Run (see RunMode)
     * @return Number of events dispatched.
     */
    uint64_t run(RunMode mode = RunMode::Drain) {
        stopped_.store(false, std::memory_order_release);
        uint64_t dispatched = 0;

        switch (mode) {
            case RunMode::Step:
                if (auto ev = queue_.tryPop()) {
                    dispatchOne(std::move(ev));
                    dispatched = 1;
                }
                break;

            case RunMode::Drain:
                while (auto ev = queue_.tryPop()) {
                    dispatchOne(std::move(ev));
                    ++dispatched;
                    ++stats_.loopIterations;
                }
                break;

            case RunMode::Run:
                while (!stopped_.load(std::memory_order_acquire)) {
                    // Blocking pop with 10 ms timeout to allow stop() checks
                    auto ev = queue_.popFor(std::chrono::milliseconds{10});
                    ++stats_.loopIterations;
                    if (!ev) continue;
                    dispatchOne(std::move(ev));
                    ++dispatched;
                }
                // Final drain after stop() signal
                while (auto ev = queue_.tryPop()) {
                    dispatchOne(std::move(ev));
                    ++dispatched;
                }
                break;
        }

        return dispatched;
    }

    /// Signal a running loop (RunMode::Run) to stop after the current event.
    void stop() noexcept {
        stopped_.store(true, std::memory_order_release);
        queue_.stop();
    }

    // ── Accessors ────────────────────────────────────────────────

    [[nodiscard]] EventQueue&      queue()      noexcept { return queue_; }
    [[nodiscard]] EventDispatcher& dispatcher() noexcept { return dispatcher_; }
    [[nodiscard]] IClock&          clock()      noexcept { return *clock_; }
    [[nodiscard]] const LoopStats& stats()      const noexcept { return stats_; }

    void resetStats() noexcept { stats_.reset(); }

    [[nodiscard]] bool isStopped() const noexcept {
        return stopped_.load(std::memory_order_acquire);
    }

private:
    void dispatchOne(std::unique_ptr<Event> ev) {
        if (preHook_) preHook_(*ev, stats_.loopIterations);

        const uint64_t t0 = static_cast<uint64_t>(nowNs());
        try {
            dispatcher_.dispatch(*ev);
        } catch (const std::exception& ex) {
            ++stats_.droppedEvents;
            Logger::instance().error("EventLoop",
                "Handler threw: {}", ex.what());
        } catch (...) {
            ++stats_.droppedEvents;
            Logger::instance().error("EventLoop", "Handler threw unknown exception");
        }
        const uint64_t elapsed = static_cast<uint64_t>(nowNs()) - t0;

        stats_.recordDispatch(ev->type, elapsed);

        if (postHook_) postHook_(*ev, elapsed);
    }

    EventQueue         queue_;
    EventDispatcher    dispatcher_;
    std::shared_ptr<IClock> clock_;
    LoopStats          stats_;
    PreDispatchHook    preHook_;
    PostDispatchHook   postHook_;
    std::atomic<bool>  stopped_{false};
};

// ─────────────────────────────────────────────────────────────
// LoopStats::report() — out-of-line to avoid circular includes
// ─────────────────────────────────────────────────────────────

inline std::string LoopStats::report() const {
    static constexpr std::array<const char*, 8> kNames{
        "Market", "Order", "Fill", "Signal",
        "Risk", "Timer", "System", "Unknown"
    };

    std::string out;
    out.reserve(512);
    out += "═══ EventLoop Statistics ═══════════════════════\n";
    out += "Total events dispatched : " + std::to_string(totalEvents)     + "\n";
    out += "Dropped (handler threw) : " + std::to_string(droppedEvents)   + "\n";
    out += "Loop iterations         : " + std::to_string(loopIterations)  + "\n";

    if (totalEvents > 0) {
        double avgUs = (static_cast<double>(totalDispatchNs) / totalEvents) / 1000.0;
        out += "Avg dispatch latency    : " + std::to_string(avgUs) + " µs\n";
    }

    out += "Per-type breakdown:\n";
    for (size_t i = 0; i < kNames.size(); ++i) {
        const auto& s = perType[i];
        if (s.count == 0) continue;
        out += "  " + std::string{kNames[i]} + " : count=" +
               std::to_string(s.count) +
               " avg=" + std::to_string(s.avgNs() / 1000.0) + "µs" +
               " min=" + std::to_string(s.minNs / 1000) + "µs" +
               " max=" + std::to_string(s.maxNs / 1000) + "µs\n";
    }
    out += "═════════════════════════════════════════════════\n";
    return out;
}

} // namespace qtl
