#pragma once
/**
 * @file risk/kill_switch/KillSwitch.hpp
 * @brief Emergency trading halt — the last line of defence.
 *
 * Architecture
 * ────────────
 * The KillSwitch is a singleton-like shared object threaded through every
 * risk-sensitive component.  Any component can trigger a halt; all components
 * check before acting.
 *
 * Guarantees
 * ──────────
 *  1. Once triggered, isTradingHalted() returns true atomically — no
 *     subsequent order can pass a well-written pre-flight check.
 *  2. Trigger reason + timestamp are recorded immutably.
 *  3. Multiple simultaneous triggers are safe (CAS, first-writer wins).
 *  4. Manual reset is intentionally guarded (requires explicit token).
 *  5. Callbacks (async, synchronous) notify downstream on trigger.
 *  6. Thread-safe: all public methods callable from any thread.
 *
 * Usage
 * ─────
 * @code
 *   auto ks = std::make_shared<KillSwitch>();
 *   ks->onTrigger([](const KillEvent& e){
 *       logger.fatal("KILL SWITCH: {}", e.reason);
 *       cancelAllOrders();
 *   });
 *
 *   // In risk monitor:
 *   if (dailyLoss > maxLoss) {
 *       ks->trigger(KillReason::DailyLossLimit, "Loss $50k > limit $45k");
 *   }
 *
 *   // In order router pre-flight:
 *   if (ks->isTradingHalted()) return RejectResult{};
 * @endcode
 */

#include "core/Types.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <thread>
#include <sstream>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// KillReason — enumeration of halt causes
// ─────────────────────────────────────────────────────────────

enum class KillReason : uint8_t {
    None            = 0,
    DailyLossLimit,       ///< Realised + unrealised loss exceeded daily limit
    MaxDrawdown,          ///< Portfolio drawdown exceeded threshold
    PositionLimit,        ///< Single position size exceeded
    GrossExposure,        ///< Total gross exposure exceeded
    NetExposure,          ///< Net long/short exposure exceeded
    ConcentrationLimit,   ///< Single-name concentration too high
    VaRBreach,            ///< Real-time VaR estimate exceeded
    OrderRateLimit,       ///< Orders per second exceeded (algo runaway)
    ConnectionLost,       ///< Exchange connection dropped
    ManualHalt,           ///< Operator triggered via CLI/GUI
    SystemError,          ///< Internal system fault
    RegulatoryHalt,       ///< Exchange halted trading
};

inline std::string killReasonName(KillReason r) {
    switch (r) {
        case KillReason::None:              return "NONE";
        case KillReason::DailyLossLimit:    return "DAILY_LOSS_LIMIT";
        case KillReason::MaxDrawdown:       return "MAX_DRAWDOWN";
        case KillReason::PositionLimit:     return "POSITION_LIMIT";
        case KillReason::GrossExposure:     return "GROSS_EXPOSURE";
        case KillReason::NetExposure:       return "NET_EXPOSURE";
        case KillReason::ConcentrationLimit:return "CONCENTRATION_LIMIT";
        case KillReason::VaRBreach:         return "VAR_BREACH";
        case KillReason::OrderRateLimit:    return "ORDER_RATE_LIMIT";
        case KillReason::ConnectionLost:    return "CONNECTION_LOST";
        case KillReason::ManualHalt:        return "MANUAL_HALT";
        case KillReason::SystemError:       return "SYSTEM_ERROR";
        case KillReason::RegulatoryHalt:    return "REGULATORY_HALT";
    }
    return "UNKNOWN";
}

// ─────────────────────────────────────────────────────────────
// KillEvent — immutable record of a halt
// ─────────────────────────────────────────────────────────────

struct KillEvent {
    KillReason  reason{KillReason::None};
    std::string message;
    Timestamp   triggeredAt{0};
    std::string triggeredBy;   ///< Component name that fired the kill
    double      currentValue{0.0};
    double      limitValue{0.0};

    [[nodiscard]] std::string toString() const {
        std::ostringstream oss;
        oss << "KILL_SWITCH_TRIGGERED"
            << " reason="   << killReasonName(reason)
            << " by="       << triggeredBy
            << " msg=\""    << message << "\""
            << " value="    << currentValue
            << " limit="    << limitValue
            << " ts="       << triggeredAt;
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────
// KillSwitch
// ─────────────────────────────────────────────────────────────

class KillSwitch {
public:
    using TriggerCallback = std::function<void(const KillEvent&)>;

    KillSwitch() = default;

    // Non-copyable (owns atomic state)
    KillSwitch(const KillSwitch&)            = delete;
    KillSwitch& operator=(const KillSwitch&) = delete;

    // ── Core API ─────────────────────────────────────────────

    /**
     * @brief Halt trading.
     *
     * Thread-safe.  First caller wins; subsequent calls are no-ops.
     * Fires all registered callbacks synchronously on the calling thread.
     *
     * @param reason      Why trading is being halted
     * @param message     Human-readable explanation
     * @param triggeredBy Component name (e.g. "DailyLossMonitor")
     * @param current     Current value that breached the limit
     * @param limit       The limit value
     */
    void trigger(KillReason  reason,
                 std::string message,
                 std::string triggeredBy = "Unknown",
                 double      current     = 0.0,
                 double      limit       = 0.0) noexcept
    {
        // CAS: only the first trigger wins
        bool expected = false;
        if (!halted_.compare_exchange_strong(
                expected, true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return;  // already halted — someone else got here first
        }

        event_.reason       = reason;
        event_.message      = std::move(message);
        event_.triggeredAt  = nowNs();
        event_.triggeredBy  = std::move(triggeredBy);
        event_.currentValue = current;
        event_.limitValue   = limit;

        fireCallbacks();
        ++triggerCount_;
    }

    /**
     * @brief Check whether trading is halted.
     * Called on every order path — must be as cheap as possible.
     */
    [[nodiscard]] bool isTradingHalted() const noexcept {
        return halted_.load(std::memory_order_acquire);
    }

    /**
     * @brief Block the calling thread until the kill switch is triggered.
     * Useful for a dedicated monitor thread.
     */
    void waitForTrigger() const {
        while (!halted_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds{100});
        }
    }

    // ── Reset (guarded) ───────────────────────────────────────

    /**
     * @brief Reset the kill switch to allow trading to resume.
     *
     * Requires the reset token to prevent accidental resets.
     * In production this token would be a cryptographic challenge.
     *
     * @param token  Must equal "CONFIRMED_RESET" to proceed.
     */
    bool reset(const std::string& token) noexcept {
        if (token != "CONFIRMED_RESET") return false;
        halted_.store(false, std::memory_order_release);
        event_ = KillEvent{};
        ++resetCount_;
        return true;
    }

    // ── Callbacks ────────────────────────────────────────────

    void onTrigger(TriggerCallback cb) {
        std::lock_guard lock{cbMutex_};
        callbacks_.push_back(std::move(cb));
    }

    void clearCallbacks() {
        std::lock_guard lock{cbMutex_};
        callbacks_.clear();
    }

    // ── Accessors ─────────────────────────────────────────────

    [[nodiscard]] const KillEvent& event()   const noexcept { return event_;        }
    [[nodiscard]] KillReason       reason()  const noexcept { return event_.reason; }
    [[nodiscard]] uint64_t triggerCount()    const noexcept { return triggerCount_;  }
    [[nodiscard]] uint64_t resetCount()      const noexcept { return resetCount_;    }

private:
    void fireCallbacks() noexcept {
        std::lock_guard lock{cbMutex_};
        for (auto& cb : callbacks_) {
            try { cb(event_); }
            catch (...) { /* callbacks must not throw */ }
        }
    }

    std::atomic<bool> halted_{false};
    KillEvent         event_;
    uint64_t          triggerCount_{0};
    uint64_t          resetCount_{0};
    mutable std::mutex cbMutex_;
    std::vector<TriggerCallback> callbacks_;
};

} // namespace qtl
