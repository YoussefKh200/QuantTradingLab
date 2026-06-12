#pragma once
/**
 * @file backtesting/replay/TickReplayer.hpp
 * @brief CSV tick replay engine — drives backtests with historical data.
 *
 * Architecture
 * ────────────
 * The TickReplayer reads one or more CSV tick files, merges them into a
 * single time-ordered stream, and replays them at controlled speed:
 *
 *   ┌──────────────┐     AnyTick     ┌──────────────────┐
 *   │ CSV File(s)  │ ─────────────► │  TickReplayer    │
 *   └──────────────┘                 │                  │
 *                                    │  - time control  │
 *                                    │  - speed factor  │
 *                                    │  - pause/resume  │
 *                                    └────────┬─────────┘
 *                                             │ AnyTick
 *                                    ┌────────▼─────────┐
 *                                    │  MarketDataFeed  │
 *                                    └────────┬─────────┘
 *                                             │ MarketEvent
 *                                    ┌────────▼─────────┐
 *                                    │   EventLoop      │
 *                                    └──────────────────┘
 *
 * Replay modes
 * ────────────
 *  • As-fast-as-possible (speed = 0): no sleep between ticks.
 *    Used for backtests — maximize throughput.
 *
 *  • Real-time (speed = 1.0): replays at exact historical intervals.
 *    Used for paper trading and latency research.
 *
 *  • Accelerated (speed > 1.0): replays N× faster than real-time.
 *    Used for intraday simulation compressed to minutes.
 *
 * Multi-file merge
 * ────────────────
 * Multiple CSV files (e.g. one per symbol, one per day) are merged by
 * priority queue on timestamp. This gives a single globally time-ordered
 * stream regardless of file order or interleaving.
 *
 * Capabilities
 * ────────────
 *   loadCSV()    — add a tick file to the replay set
 *   replay()     — start replay loop (blocking or background thread)
 *   pause()      — suspend replay (keeps position)
 *   resume()     — continue from paused position
 *   seek()       — jump to a timestamp
 *   stop()       — terminate replay and clean up
 *   progress()   — fraction of ticks replayed [0.0, 1.0]
 */

#include "exchange/marketdata/Tick.hpp"
#include "exchange/marketdata/TickParser.hpp"
#include "exchange/marketdata/MarketDataFeed.hpp"
#include "core/clock/Clock.hpp"
#include "core/logger/Logger.hpp"

#include <string>
#include <vector>
#include <queue>
#include <fstream>
#include <sstream>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <memory>
#include <chrono>
#include <algorithm>
#include <stdexcept>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// ReplayStats
// ─────────────────────────────────────────────────────────────

struct ReplayStats {
    uint64_t  ticksReplayed{0};
    uint64_t  ticksTotal{0};       ///< Total ticks loaded (0 if streaming)
    uint64_t  parseErrors{0};
    Timestamp replayStartWall{0};  ///< Wall-clock time replay began
    Timestamp replayEndWall{0};
    Timestamp dataStartTime{0};    ///< First tick timestamp in data
    Timestamp dataEndTime{0};      ///< Last tick timestamp in data
    double    speedFactor{0.0};

    [[nodiscard]] double progress() const noexcept {
        return ticksTotal > 0
                   ? static_cast<double>(ticksReplayed) /
                         static_cast<double>(ticksTotal)
                   : 0.0;
    }
    [[nodiscard]] double wallDurationSec() const noexcept {
        if (replayEndWall <= replayStartWall) return 0.0;
        return static_cast<double>(replayEndWall - replayStartWall) / 1e9;
    }
    [[nodiscard]] double dataDurationSec() const noexcept {
        return static_cast<double>(dataEndTime - dataStartTime) / 1e9;
    }
    [[nodiscard]] double actualSpeedFactor() const noexcept {
        double wall = wallDurationSec();
        double data = dataDurationSec();
        return (wall > 0 && data > 0) ? data / wall : 0.0;
    }
};

// ─────────────────────────────────────────────────────────────
// TickSource — one loaded CSV file as an iterator
// ─────────────────────────────────────────────────────────────

class TickSource {
public:
    explicit TickSource(const std::string& path, TickType type)
        : path_{path}, type_{type}
    {
        file_.open(path);
        if (!file_.is_open())
            throw std::runtime_error("TickReplayer: cannot open " + path);

        // Read and detect header
        std::string headerLine;
        if (std::getline(file_, headerLine)) {
            if (TickParser::isHeaderOrComment(headerLine)) {
                detectedType_ = TickParser::detectType(headerLine);
                headerSkipped_ = true;
            } else {
                // No header — put line back by storing it
                pendingLine_ = headerLine;
                detectedType_ = type_;
            }
        }
        // If caller specified a type, override detection
        if (type_ != TickType::Quote) detectedType_ = type_;
        else                           type_         = detectedType_;

        advance();  // load first tick
    }

    [[nodiscard]] bool        valid()    const noexcept { return hasNext_;    }
    [[nodiscard]] const AnyTick& peek()  const noexcept { return current_;    }
    [[nodiscard]] Timestamp timestamp()  const noexcept {
        return hasNext_ ? current_.timestamp() : INT64_MAX;
    }

    /// Move to next tick. Returns false when exhausted.
    bool advance() {
        std::string line;

        // First consume any pre-buffered line
        if (!pendingLine_.empty()) {
            line = std::move(pendingLine_);
            pendingLine_.clear();
        } else {
            if (!std::getline(file_, line)) {
                hasNext_ = false;
                return false;
            }
        }

        // Skip blank lines and comments
        while (TickParser::isHeaderOrComment(line)) {
            if (!std::getline(file_, line)) {
                hasNext_ = false;
                return false;
            }
        }

        try {
            current_ = TickParser::parseLine(line, detectedType_);
            hasNext_ = true;
        } catch (const std::exception& ex) {
            ++parseErrors_;
            hasNext_ = false;
        }
        return hasNext_;
    }

    [[nodiscard]] uint64_t parseErrors() const noexcept { return parseErrors_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    std::string  path_;
    TickType     type_;
    TickType     detectedType_{TickType::Quote};
    std::ifstream file_;
    AnyTick      current_;
    std::string  pendingLine_;
    bool         hasNext_{false};
    bool         headerSkipped_{false};
    uint64_t     parseErrors_{0};
};

// ─────────────────────────────────────────────────────────────
// Merge comparator — min-heap on timestamp
// ─────────────────────────────────────────────────────────────

struct SourcePtrCmp {
    bool operator()(const TickSource* a, const TickSource* b) const {
        return a->timestamp() > b->timestamp();  // min-heap
    }
};

// ─────────────────────────────────────────────────────────────
// TickReplayer
// ─────────────────────────────────────────────────────────────

class TickReplayer {
public:
    explicit TickReplayer(MarketDataFeed* feed,
                           SimClock*       clock = nullptr,
                           double          speedFactor = 0.0)
        : feed_{feed}
        , simClock_{clock}
        , speedFactor_{speedFactor}
    {
        if (!feed_) throw std::invalid_argument("TickReplayer: feed is null");
    }

    ~TickReplayer() { stop(); }

    // Non-copyable
    TickReplayer(const TickReplayer&)            = delete;
    TickReplayer& operator=(const TickReplayer&) = delete;

    // ── File loading ──────────────────────────────────────────

    /**
     * @brief Load a CSV tick file into the replay set.
     *
     * @param path        Path to CSV file.
     * @param type        Tick type (Quote/Trade/BookUpdate).
     *                    Pass TickType::Quote to auto-detect from header.
     */
    void loadCSV(const std::string& path,
                  TickType type = TickType::Quote) {
        if (replaying_.load()) {
            throw std::runtime_error("Cannot load CSV while replay is running");
        }
        sources_.emplace_back(std::make_unique<TickSource>(path, type));
        Logger::instance().info("TickReplayer",
            "Loaded '{}' type={}", path,
            type == TickType::Trade ? "Trade" :
            type == TickType::Quote ? "Quote" : "BookUpdate");
    }

    // ── Replay control ────────────────────────────────────────

    /**
     * @brief Start replaying all loaded CSV files.
     *
     * @param background  If true, runs on a background thread and returns
     *                    immediately.  If false, blocks until complete.
     */
    void replay(bool background = false) {
        if (sources_.empty())
            throw std::runtime_error("TickReplayer: no CSV files loaded");

        // Build priority queue from all sources
        buildHeap();

        replaying_.store(true);
        paused_.store(false);
        stopped_.store(false);
        stats_ = ReplayStats{};
        stats_.speedFactor   = speedFactor_;
        stats_.replayStartWall = nowNs();

        if (background) {
            replayThread_ = std::thread([this]{ replayLoop(); });
        } else {
            replayLoop();
        }
    }

    /// Suspend replay at the current position.
    void pause() noexcept {
        paused_.store(true, std::memory_order_release);
        Logger::instance().info("TickReplayer", "Paused at tick {}",
                                stats_.ticksReplayed);
    }

    /// Resume from paused state.
    void resume() noexcept {
        paused_.store(false, std::memory_order_release);
        cv_.notify_all();
        Logger::instance().info("TickReplayer", "Resumed");
    }

    /// Stop replay and join background thread.
    void stop() noexcept {
        stopped_.store(true, std::memory_order_release);
        paused_.store(false, std::memory_order_release);
        cv_.notify_all();
        if (replayThread_.joinable()) replayThread_.join();
        replaying_.store(false);
    }

    /**
     * @brief Seek to the first tick at or after @p targetTs.
     * Must be called before replay() starts.
     * Scans forward (O(n)) — use only for coarse seeks.
     */
    void seek(Timestamp targetTs) {
        while (!heap_.empty()) {
            TickSource* src = heap_.top();
            if (src->timestamp() >= targetTs) break;
            heap_.pop();
            if (src->advance()) heap_.push(src);
        }
        Logger::instance().info("TickReplayer",
            "Seeked to ts={}", targetTs);
    }

    // ── Queries ──────────────────────────────────────────────

    [[nodiscard]] bool isReplaying() const noexcept {
        return replaying_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool isPaused()    const noexcept {
        return paused_.load(std::memory_order_acquire);
    }
    [[nodiscard]] const ReplayStats& stats() const noexcept { return stats_; }
    [[nodiscard]] double progress()          const noexcept {
        return stats_.progress();
    }

    /// Block until replay completes (use after background replay()).
    void waitForCompletion() {
        if (replayThread_.joinable()) replayThread_.join();
    }

    /// Register a callback fired for every tick (in addition to feed).
    void setTickCallback(std::function<void(const AnyTick&)> cb) {
        tickCb_ = std::move(cb);
    }

private:
    void buildHeap() {
        // Drain and rebuild
        while (!heap_.empty()) heap_.pop();
        for (auto& src : sources_) {
            if (src->valid()) heap_.push(src.get());
        }
    }

    void replayLoop() {
        Timestamp prevDataTs   = 0;
        Timestamp replayStartWall = nowNs();

        while (!stopped_.load(std::memory_order_acquire) && !heap_.empty()) {
            // Pause check
            if (paused_.load(std::memory_order_acquire)) {
                std::unique_lock lock{mutex_};
                cv_.wait(lock, [this]{
                    return !paused_.load() || stopped_.load();
                });
                if (stopped_.load()) break;
                // Reset timing reference after resume
                replayStartWall = nowNs() -
                    static_cast<Timestamp>(
                        static_cast<double>(
                            heap_.top()->timestamp() - stats_.dataStartTime)
                        / (speedFactor_ > 0 ? speedFactor_ : 1.0));
            }

            // Pop minimum-timestamp source
            TickSource* src = heap_.top();
            heap_.pop();

            if (!src->valid()) continue;

            const AnyTick& tick = src->peek();
            Timestamp dataTs    = tick.timestamp();

            // Update sim clock
            if (simClock_) simClock_->setTime(dataTs);

            // Timing: sleep to maintain speed factor
            if (speedFactor_ > 0.0 && prevDataTs > 0) {
                int64_t dataElapsed  = dataTs - prevDataTs;
                int64_t wallElapsed  = static_cast<int64_t>(
                    static_cast<double>(dataElapsed) / speedFactor_);
                Timestamp wakeWall   = replayStartWall +
                    static_cast<int64_t>(
                        static_cast<double>(dataTs - stats_.dataStartTime) /
                        speedFactor_);
                Timestamp nowW = nowNs();
                if (wakeWall > nowW) {
                    std::this_thread::sleep_for(
                        std::chrono::nanoseconds{wakeWall - nowW});
                }
            }

            if (stats_.dataStartTime == 0) stats_.dataStartTime = dataTs;
            stats_.dataEndTime = dataTs;
            prevDataTs = dataTs;

            // Dispatch to feed and optional callback
            feed_->processTick(tick);
            if (tickCb_) tickCb_(tick);

            ++stats_.ticksReplayed;

            // Advance source and re-insert into heap
            if (src->advance()) heap_.push(src);
            stats_.parseErrors += src->parseErrors();
        }

        stats_.replayEndWall = nowNs();
        replaying_.store(false);

        Logger::instance().info("TickReplayer",
            "Replay complete: {} ticks in {:.2f}s  ({:.1f}x speed)",
            stats_.ticksReplayed,
            stats_.wallDurationSec(),
            stats_.actualSpeedFactor());
    }

    MarketDataFeed*  feed_;
    SimClock*        simClock_;
    double           speedFactor_;

    std::vector<std::unique_ptr<TickSource>> sources_;
    std::priority_queue<
        TickSource*,
        std::vector<TickSource*>,
        SourcePtrCmp>  heap_;

    std::atomic<bool>       replaying_{false};
    std::atomic<bool>       paused_{false};
    std::atomic<bool>       stopped_{false};
    std::mutex              mutex_;
    std::condition_variable cv_;
    std::thread             replayThread_;
    ReplayStats             stats_;

    std::function<void(const AnyTick&)> tickCb_;
};

} // namespace qtl
