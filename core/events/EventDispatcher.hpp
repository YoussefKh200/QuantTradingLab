#pragma once
/**
 * @file core/events/EventDispatcher.hpp
 * @brief Typed event dispatcher with per-EventType handler registration.
 *
 * Usage pattern:
 * @code
 *   EventDispatcher dispatcher;
 *   dispatcher.subscribe<MarketEvent>([](const MarketEvent& e){
 *       // handle tick
 *   });
 *   // In simulation loop:
 *   while (queue.hasEvents()) {
 *       auto ev = queue.tryPop();
 *       if (ev) dispatcher.dispatch(*ev);
 *   }
 * @endcode
 *
 * Design:
 *  - Handlers are stored as std::function; zero-cost for simple lambdas.
 *  - Multiple handlers per EventType are supported (fan-out).
 *  - dispatch() is O(n) in handlers registered for that EventType.
 *  - Thread-safety: register all handlers before starting the event loop;
 *    runtime registration from multiple threads requires external locking.
 */

#include "core/events/Event.hpp"
#include <functional>
#include <unordered_map>
#include <vector>
#include <typeindex>
#include <memory>
#include <stdexcept>

namespace qtl {

class EventDispatcher {
public:
    /// Handler callable signature: receives a const-ref to the base Event.
    using Handler = std::function<void(const Event&)>;

    EventDispatcher() = default;

    // ─── Registration ──────────────────────────────────────────

    /**
     * @brief Register a typed handler for event subtype T.
     *
     * The dispatcher will static_cast to T& before invoking the handler,
     * so the cast is safe as long as EventType discriminators are correct.
     *
     * @tparam T  Concrete event subtype (MarketEvent, FillEvent, …)
     * @param handler  Callable that accepts const T&
     */
    template<typename T>
    void subscribe(std::function<void(const T&)> handler) {
        static_assert(std::is_base_of_v<Event, T>,
                      "T must derive from Event");
        // Wrap typed handler in a type-erased lambda that performs the cast.
        handlers_[std::type_index(typeid(T))].emplace_back(
            [h = std::move(handler)](const Event& ev) {
                h(static_cast<const T&>(ev));
            });
    }

    /**
     * @brief Register a raw (untyped) handler for a specific EventType enum.
     *
     * Useful when you want to intercept all events of a logical category
     * without caring about the concrete C++ type.
     */
    void subscribeRaw(EventType type, Handler handler) {
        rawHandlers_[static_cast<uint8_t>(type)].emplace_back(
            std::move(handler));
    }

    // ─── Dispatch ─────────────────────────────────────────────

    /**
     * @brief Dispatch a single event to all registered handlers.
     *
     * Invokes both typed handlers (matched by typeid) and raw handlers
     * (matched by EventType enum).
     */
    void dispatch(const Event& ev) {
        // Typed handlers
        auto it = handlers_.find(std::type_index(typeid(ev)));
        if (it != handlers_.end()) {
            for (auto& h : it->second) h(ev);
        }
        // Raw handlers by EventType enum
        auto ri = rawHandlers_.find(static_cast<uint8_t>(ev.type));
        if (ri != rawHandlers_.end()) {
            for (auto& h : ri->second) h(ev);
        }
    }

    /**
     * @brief Convenience: drain an entire EventQueue, dispatching each event.
     *
     * Processes all events currently in the queue (non-blocking).
     * Returns the number of events dispatched.
     */
    template<typename Queue>
    size_t drainQueue(Queue& queue) {
        size_t count = 0;
        while (auto ev = queue.tryPop()) {
            dispatch(*ev);
            ++count;
        }
        return count;
    }

    /// Remove all handlers.
    void clear() noexcept {
        handlers_.clear();
        rawHandlers_.clear();
    }

    /// Number of typed handler lists registered.
    [[nodiscard]] size_t handlerCount() const noexcept {
        size_t total = 0;
        for (auto& [k,v] : handlers_) total += v.size();
        for (auto& [k,v] : rawHandlers_) total += v.size();
        return total;
    }

private:
    /// typeid → list of type-erased handlers
    std::unordered_map<std::type_index, std::vector<Handler>> handlers_;
    /// EventType enum → list of raw handlers
    std::unordered_map<uint8_t, std::vector<Handler>> rawHandlers_;
};

} // namespace qtl
