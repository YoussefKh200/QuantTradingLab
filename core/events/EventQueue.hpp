#pragma once
/**
 * @file core/events/EventQueue.hpp
 * @brief Thread-safe, bounded event queue with optional blocking pop.
 *
 * Architecture:
 *  - Two queues are provided:
 *    1. EventQueue      – mutex + condition_variable; safe for multi-producer
 *       multi-consumer across threads (back-test replay, order routing).
 *    2. SPSCEventQueue  – lock-free single-producer / single-consumer ring
 *       buffer for the hot path (market data → strategy).
 *
 *  For Phase 1 we implement EventQueue.  SPSCEventQueue is wired in Phase 12
 *  (low-latency components) and swapped in via a template policy parameter.
 */

#include "core/events/Event.hpp"
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <optional>
#include <chrono>
#include <atomic>

namespace qtl {

/**
 * @class EventQueue
 * @brief Unbounded, mutex-protected MPMC event queue.
 *
 * Producers call push(); consumers call pop() or tryPop().
 * The queue owns events via unique_ptr to ensure single-owner semantics.
 */
class EventQueue {
public:
    /// Maximum events stored before push blocks (0 = unlimited)
    static constexpr size_t kDefaultCapacity = 0;

    EventQueue() = default;
    ~EventQueue() = default;

    // Non-copyable, non-movable (owns a mutex)
    EventQueue(const EventQueue&)            = delete;
    EventQueue& operator=(const EventQueue&) = delete;

    // ─── Producer interface ────────────────────────────────────

    /**
     * @brief Push an event onto the queue.
     *
     * Takes ownership of the unique_ptr.  Notifies one waiting consumer.
     * Thread-safe.
     */
    void push(std::unique_ptr<Event> event) {
        {
            std::lock_guard lock{mutex_};
            queue_.push(std::move(event));
            ++totalPushed_;
        }
        cv_.notify_one();
    }

    // ─── Consumer interface ────────────────────────────────────

    /**
     * @brief Blocking pop.  Waits until an event is available or the queue
     *        is stopped.
     * @return Event pointer, or nullptr if the queue was stopped.
     */
    [[nodiscard]] std::unique_ptr<Event> pop() {
        std::unique_lock lock{mutex_};
        cv_.wait(lock, [this]{ return !queue_.empty() || stopped_; });
        if (queue_.empty()) return nullptr;  // stopped
        auto ev = std::move(queue_.front());
        queue_.pop();
        ++totalPopped_;
        return ev;
    }

    /**
     * @brief Non-blocking pop.
     * @return Event or std::nullopt if empty.
     */
    [[nodiscard]] std::unique_ptr<Event> tryPop() {
        std::lock_guard lock{mutex_};
        if (queue_.empty()) return nullptr;
        auto ev = std::move(queue_.front());
        queue_.pop();
        ++totalPopped_;
        return ev;
    }

    /**
     * @brief Timed pop — waits up to @p timeout for an event.
     */
    template<typename Rep, typename Period>
    [[nodiscard]] std::unique_ptr<Event>
    popFor(const std::chrono::duration<Rep,Period>& timeout) {
        std::unique_lock lock{mutex_};
        if (!cv_.wait_for(lock, timeout,
                          [this]{ return !queue_.empty() || stopped_; })) {
            return nullptr;  // timed out
        }
        if (queue_.empty()) return nullptr;
        auto ev = std::move(queue_.front());
        queue_.pop();
        ++totalPopped_;
        return ev;
    }

    // ─── State queries ─────────────────────────────────────────

    [[nodiscard]] bool hasEvents() const noexcept {
        std::lock_guard lock{mutex_};
        return !queue_.empty();
    }

    [[nodiscard]] size_t size() const noexcept {
        std::lock_guard lock{mutex_};
        return queue_.size();
    }

    [[nodiscard]] bool empty() const noexcept {
        std::lock_guard lock{mutex_};
        return queue_.empty();
    }

    /// Signal all waiting consumers to wake up and return nullptr.
    void stop() noexcept {
        {
            std::lock_guard lock{mutex_};
            stopped_ = true;
        }
        cv_.notify_all();
    }

    [[nodiscard]] bool isStopped() const noexcept { return stopped_.load(); }

    // ─── Statistics ────────────────────────────────────────────

    [[nodiscard]] uint64_t totalPushed() const noexcept {
        return totalPushed_.load();
    }
    [[nodiscard]] uint64_t totalPopped() const noexcept {
        return totalPopped_.load();
    }

private:
    mutable std::mutex          mutex_;
    std::condition_variable     cv_;
    std::queue<std::unique_ptr<Event>> queue_;
    std::atomic<bool>           stopped_{false};
    std::atomic<uint64_t>       totalPushed_{0};
    std::atomic<uint64_t>       totalPopped_{0};
};

} // namespace qtl
