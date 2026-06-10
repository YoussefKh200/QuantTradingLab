#pragma once
/**
 * @file core/clock/Clock.hpp
 * @brief Unified clock interface for wall-clock and simulated time.
 *
 * In a backtesting / simulation context the clock must be controllable:
 *  - SimClock advances by injecting timestamps from replay data.
 *  - WallClock wraps std::chrono::high_resolution_clock.
 *
 * Both implement IClock so strategies are clock-agnostic and can be
 * tested in fast-forward without any code changes.
 */

#include "core/Types.hpp"
#include <atomic>
#include <chrono>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// Interface
// ─────────────────────────────────────────────────────────────

class IClock {
public:
    virtual ~IClock() = default;

    /// Current time in nanoseconds since Unix epoch.
    [[nodiscard]] virtual Timestamp now() const noexcept = 0;

    /// Elapsed nanoseconds since the clock was created / reset.
    [[nodiscard]] virtual int64_t elapsedNs() const noexcept = 0;
};

// ─────────────────────────────────────────────────────────────
// WallClock — real-time, wraps high_resolution_clock
// ─────────────────────────────────────────────────────────────

class WallClock final : public IClock {
public:
    WallClock() noexcept
        : startNs_{nowNs()} {}

    [[nodiscard]] Timestamp now() const noexcept override {
        return nowNs();
    }

    [[nodiscard]] int64_t elapsedNs() const noexcept override {
        return nowNs() - startNs_;
    }

    void reset() noexcept { startNs_ = nowNs(); }

private:
    Timestamp startNs_;
};

// ─────────────────────────────────────────────────────────────
// SimClock — controllable clock for backtesting
//
// The replay engine calls setTime() as it feeds ticks;
// strategies read now() and see the simulated exchange time.
// ─────────────────────────────────────────────────────────────

class SimClock final : public IClock {
public:
    explicit SimClock(Timestamp startNs = 0) noexcept
        : currentNs_{startNs}, startNs_{startNs} {}

    [[nodiscard]] Timestamp now() const noexcept override {
        return currentNs_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int64_t elapsedNs() const noexcept override {
        return currentNs_.load(std::memory_order_relaxed) - startNs_;
    }

    /// Advance the simulated clock to @p ts (must be >= current time).
    void setTime(Timestamp ts) noexcept {
        currentNs_.store(ts, std::memory_order_release);
    }

    /// Advance by @p deltaNs nanoseconds.
    void advance(int64_t deltaNs) noexcept {
        currentNs_.fetch_add(deltaNs, std::memory_order_acq_rel);
    }

    void reset(Timestamp newStart = 0) noexcept {
        startNs_ = newStart;
        currentNs_.store(newStart, std::memory_order_release);
    }

private:
    std::atomic<Timestamp> currentNs_;
    Timestamp              startNs_;
};

} // namespace qtl
