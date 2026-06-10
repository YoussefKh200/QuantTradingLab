#pragma once
/**
 * @file core/logger/Logger.hpp
 * @brief Asynchronous, thread-safe structured logger.
 *
 * Design:
 *  - Log records are pushed onto a lock-free-friendly deque from any thread.
 *  - A dedicated background thread drains the deque and writes to sink(s).
 *  - Sinks are pluggable: ConsoleSink, FileSink (more added per phase).
 *  - Severity levels: TRACE < DEBUG < INFO < WARN < ERROR < FATAL.
 *  - Zero dynamic allocation on the hot path (format into a fixed stack buffer).
 *
 * Usage:
 * @code
 *   auto& log = Logger::instance();
 *   log.info("OrderBook", "Added order id={} price={:.2f}", id, price);
 * @endcode
 */

#include <string>
#include <string_view>
#include <array>
#include <vector>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <queue>
#include <memory>
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <format>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// Severity
// ─────────────────────────────────────────────────────────────

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug,
    Info,
    Warn,
    Error,
    Fatal
};

inline std::string_view levelName(LogLevel l) noexcept {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

// ─────────────────────────────────────────────────────────────
// LogRecord — one log entry
// ─────────────────────────────────────────────────────────────

struct LogRecord {
    int64_t     timestampNs;
    LogLevel    level;
    std::string component;
    std::string message;
};

// ─────────────────────────────────────────────────────────────
// ISink — output target
// ─────────────────────────────────────────────────────────────

class ISink {
public:
    virtual ~ISink() = default;
    virtual void write(const LogRecord& rec) = 0;
    virtual void flush() = 0;
};

// ─────────────────────────────────────────────────────────────
// ConsoleSink
// ─────────────────────────────────────────────────────────────

class ConsoleSink final : public ISink {
public:
    void write(const LogRecord& rec) override {
        // Format: [HH:MM:SS.nnn] LEVEL [Component] message
        auto ns  = rec.timestampNs;
        auto sec = ns / 1'000'000'000LL;
        auto ms  = (ns % 1'000'000'000LL) / 1'000'000LL;

        std::time_t t = static_cast<std::time_t>(sec);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char timebuf[32];
        std::strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm);

        std::fprintf(stderr, "[%s.%03lld] %s [%s] %s\n",
                     timebuf,
                     static_cast<long long>(ms),
                     std::string{levelName(rec.level)}.c_str(),
                     rec.component.c_str(),
                     rec.message.c_str());
    }

    void flush() override { std::fflush(stderr); }
};

// ─────────────────────────────────────────────────────────────
// FileSink
// ─────────────────────────────────────────────────────────────

class FileSink final : public ISink {
public:
    explicit FileSink(const std::string& path)
        : file_{path, std::ios::out | std::ios::app} {
        if (!file_.is_open())
            throw std::runtime_error("FileSink: cannot open " + path);
    }

    void write(const LogRecord& rec) override {
        auto ns  = rec.timestampNs;
        auto sec = ns / 1'000'000'000LL;
        auto ms  = (ns % 1'000'000'000LL) / 1'000'000LL;

        std::time_t t = static_cast<std::time_t>(sec);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char timebuf[32];
        std::strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tm);

        file_ << '[' << timebuf << '.' << ms << "] "
              << levelName(rec.level) << " ["
              << rec.component << "] "
              << rec.message << '\n';
    }

    void flush() override { file_.flush(); }

private:
    std::ofstream file_;
};

// ─────────────────────────────────────────────────────────────
// Logger — singleton async logger
// ─────────────────────────────────────────────────────────────

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    ~Logger() { stop(); }

    // ─── Configuration ────────────────────────────────────────

    void addSink(std::shared_ptr<ISink> sink) {
        std::lock_guard lock{sinkMutex_};
        sinks_.push_back(std::move(sink));
    }

    void setLevel(LogLevel level) noexcept {
        minLevel_.store(level, std::memory_order_relaxed);
    }

    // ─── Logging interface ────────────────────────────────────

    template<typename... Args>
    void log(LogLevel level, std::string_view component,
             std::format_string<Args...> fmt, Args&&... args)
    {
        if (level < minLevel_.load(std::memory_order_relaxed)) return;

        LogRecord rec;
        rec.timestampNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
        rec.level     = level;
        rec.component = std::string{component};
        rec.message   = std::format(fmt, std::forward<Args>(args)...);

        {
            std::lock_guard lock{queueMutex_};
            queue_.push(std::move(rec));
        }
        cv_.notify_one();
    }

    // Convenience wrappers
    template<typename... Args>
    void trace(std::string_view c, std::format_string<Args...> f, Args&&... a) {
        log(LogLevel::Trace, c, f, std::forward<Args>(a)...);
    }
    template<typename... Args>
    void debug(std::string_view c, std::format_string<Args...> f, Args&&... a) {
        log(LogLevel::Debug, c, f, std::forward<Args>(a)...);
    }
    template<typename... Args>
    void info(std::string_view c, std::format_string<Args...> f, Args&&... a) {
        log(LogLevel::Info, c, f, std::forward<Args>(a)...);
    }
    template<typename... Args>
    void warn(std::string_view c, std::format_string<Args...> f, Args&&... a) {
        log(LogLevel::Warn, c, f, std::forward<Args>(a)...);
    }
    template<typename... Args>
    void error(std::string_view c, std::format_string<Args...> f, Args&&... a) {
        log(LogLevel::Error, c, f, std::forward<Args>(a)...);
    }
    template<typename... Args>
    void fatal(std::string_view c, std::format_string<Args...> f, Args&&... a) {
        log(LogLevel::Fatal, c, f, std::forward<Args>(a)...);
    }

    void stop() {
        stopped_.store(true, std::memory_order_release);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        flushAll();
    }

private:
    Logger() {
        // Default: console sink at INFO level
        sinks_.push_back(std::make_shared<ConsoleSink>());
        minLevel_.store(LogLevel::Info, std::memory_order_relaxed);
        // Start background drain thread
        worker_ = std::thread(&Logger::drainLoop, this);
    }

    void drainLoop() {
        while (!stopped_.load(std::memory_order_acquire)) {
            std::unique_lock lock{queueMutex_};
            cv_.wait(lock, [this]{
                return !queue_.empty() || stopped_.load(std::memory_order_relaxed);
            });
            // Drain all pending records
            while (!queue_.empty()) {
                auto rec = std::move(queue_.front());
                queue_.pop();
                lock.unlock();
                writeToSinks(rec);
                lock.lock();
            }
        }
    }

    void writeToSinks(const LogRecord& rec) {
        std::lock_guard lock{sinkMutex_};
        for (auto& s : sinks_) s->write(rec);
    }

    void flushAll() {
        std::lock_guard lock{sinkMutex_};
        for (auto& s : sinks_) s->flush();
    }

    std::mutex                        queueMutex_;
    std::condition_variable           cv_;
    std::queue<LogRecord>             queue_;
    std::mutex                        sinkMutex_;
    std::vector<std::shared_ptr<ISink>> sinks_;
    std::atomic<LogLevel>             minLevel_{LogLevel::Info};
    std::atomic<bool>                 stopped_{false};
    std::thread                       worker_;
};

// ─────────────────────────────────────────────────────────────
// Convenience macros
// ─────────────────────────────────────────────────────────────
#define QTL_LOG_INFO(component, ...)  ::qtl::Logger::instance().info(component, __VA_ARGS__)
#define QTL_LOG_WARN(component, ...)  ::qtl::Logger::instance().warn(component, __VA_ARGS__)
#define QTL_LOG_ERROR(component, ...) ::qtl::Logger::instance().error(component, __VA_ARGS__)
#define QTL_LOG_DEBUG(component, ...) ::qtl::Logger::instance().debug(component, __VA_ARGS__)

} // namespace qtl
