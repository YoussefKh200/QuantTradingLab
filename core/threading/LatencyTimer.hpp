#pragma once
/**
 * @file core/threading/LatencyTimer.hpp
 * @brief High-resolution latency measurement and histogram reporting.
 *
 * Components
 * ──────────
 * LatencyTimer   — RAII scoped timer; records one sample on destruction.
 * LatencyStats   — Online statistics: min/max/mean/p50/p95/p99/p999.
 * LatencyBench   — Benchmark harness: run N iterations, report full stats.
 *
 * Design
 * ──────
 * Uses std::chrono::high_resolution_clock for portability.
 * For production HFT use rdtsc directly; this layer abstracts the choice.
 *
 * Histogram uses 8 logarithmic buckets (ns):
 *   <100ns, <1µs, <10µs, <100µs, <1ms, <10ms, <100ms, ≥100ms
 *
 * Percentiles computed from sorted sample buffer (reservoir sampling
 * when sample count exceeds maxSamples).
 */

#include <chrono>
#include <atomic>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <functional>
#include <cmath>
#include <cstdint>
#include <array>
#include <mutex>

namespace qtl {

// ─────────────────────────────────────────────────────────────
// Timing primitives
// ─────────────────────────────────────────────────────────────

/// Return current time in nanoseconds (high-resolution wall clock).
[[nodiscard]] inline int64_t hrnow() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()
    ).count();
}

// ─────────────────────────────────────────────────────────────
// LatencyStats — online accumulator + histogram
// ─────────────────────────────────────────────────────────────

class LatencyStats {
public:
    static constexpr size_t kMaxSamples = 100'000;
    static constexpr size_t kBuckets    = 8;

    explicit LatencyStats(std::string name = "")
        : name_{std::move(name)} {
        samples_.reserve(kMaxSamples);
    }

    void record(int64_t ns) noexcept {
        if (ns < 0) ns = 0;
        ++count_;
        sum_   += static_cast<uint64_t>(ns);
        sumSq_ += static_cast<double>(ns) * static_cast<double>(ns);
        if (ns < minNs_) minNs_ = ns;
        if (ns > maxNs_) maxNs_ = ns;

        // Histogram bucket
        size_t b = 0;
        if      (ns <           100) b = 0;
        else if (ns <         1'000) b = 1;
        else if (ns <        10'000) b = 2;
        else if (ns <       100'000) b = 3;
        else if (ns <     1'000'000) b = 4;
        else if (ns <    10'000'000) b = 5;
        else if (ns <   100'000'000) b = 6;
        else                         b = 7;
        ++buckets_[b];

        // Reservoir: keep up to kMaxSamples
        std::lock_guard lock{mu_};
        if (samples_.size() < kMaxSamples) {
            samples_.push_back(static_cast<uint64_t>(ns));
        } else {
            // Replace random element (reservoir sampling)
            size_t idx = static_cast<size_t>(count_) % kMaxSamples;
            samples_[idx] = static_cast<uint64_t>(ns);
        }
    }

    LatencyStats(LatencyStats&& o) noexcept
        : name_{std::move(o.name_)}
        , count_{o.count_}
        , sum_{o.sum_}
        , sumSq_{o.sumSq_}
        , minNs_{o.minNs_}
        , maxNs_{o.maxNs_}
        , buckets_{o.buckets_}
        , samples_{std::move(o.samples_)}
    { o.count_ = 0; }

    LatencyStats& operator=(LatencyStats&& o) noexcept {
        if (this != &o) {
            name_ = std::move(o.name_);
            count_ = o.count_; sum_ = o.sum_; sumSq_ = o.sumSq_;
            minNs_ = o.minNs_; maxNs_ = o.maxNs_; buckets_ = o.buckets_;
            std::lock_guard lock{mu_};
            samples_ = std::move(o.samples_);
            o.count_ = 0;
        }
        return *this;
    }

    void reset() noexcept {
        count_ = 0; sum_ = 0; sumSq_ = 0;
        minNs_ = INT64_MAX; maxNs_ = 0;
        buckets_.fill(0);
        std::lock_guard lock{mu_};
        samples_.clear();
    }

    // ── Query ─────────────────────────────────────────────────

    [[nodiscard]] uint64_t count()  const noexcept { return count_;  }
    [[nodiscard]] int64_t  minNs()  const noexcept { return minNs_;  }
    [[nodiscard]] int64_t  maxNs()  const noexcept { return maxNs_;  }
    [[nodiscard]] double   meanNs() const noexcept {
        return count_ > 0 ? static_cast<double>(sum_) / count_ : 0.0;
    }
    [[nodiscard]] double   stddevNs() const noexcept {
        if (count_ < 2) return 0.0;
        double m = meanNs();
        double v = sumSq_ / count_ - m * m;
        return v > 0 ? std::sqrt(v) : 0.0;
    }
    [[nodiscard]] double   meanUs()  const noexcept { return meanNs()  / 1000.0; }
    [[nodiscard]] double   minUs()   const noexcept { return minNs_    / 1000.0; }
    [[nodiscard]] double   maxUs()   const noexcept { return maxNs_    / 1000.0; }

    [[nodiscard]] double percentile(double pct) const {
        std::lock_guard lock{mu_};
        if (samples_.empty()) return 0.0;
        std::vector<uint64_t> sorted = samples_;
        std::sort(sorted.begin(), sorted.end());
        size_t idx = static_cast<size_t>(pct / 100.0 * sorted.size());
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        return static_cast<double>(sorted[idx]);
    }

    [[nodiscard]] double p50()  const { return percentile(50.0);  }
    [[nodiscard]] double p95()  const { return percentile(95.0);  }
    [[nodiscard]] double p99()  const { return percentile(99.0);  }
    [[nodiscard]] double p999() const { return percentile(99.9);  }

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    // ── Report ────────────────────────────────────────────────

    [[nodiscard]] std::string report() const {
        static constexpr std::array<const char*, kBuckets> kLabels{
            "<100ns","<1µs","<10µs","<100µs","<1ms","<10ms","<100ms","≥100ms"
        };
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);
        oss << "── " << (name_.empty() ? "Latency" : name_) << " Stats ──\n"
            << "  count  : " << count_          << "\n"
            << "  min    : " << minNs_/1000.0   << " µs\n"
            << "  mean   : " << meanUs()         << " µs\n"
            << "  stddev : " << stddevNs()/1000  << " µs\n"
            << "  p50    : " << p50()/1000.0     << " µs\n"
            << "  p95    : " << p95()/1000.0     << " µs\n"
            << "  p99    : " << p99()/1000.0     << " µs\n"
            << "  p99.9  : " << p999()/1000.0    << " µs\n"
            << "  max    : " << maxNs_/1000.0    << " µs\n"
            << "  histogram:\n";
        for (size_t i = 0; i < kBuckets; ++i) {
            if (buckets_[i] == 0) continue;
            oss << "    " << kLabels[i] << " : " << buckets_[i]
                << " (" << (count_ > 0
                    ? std::to_string(buckets_[i]*100/count_) : "0")
                << "%)\n";
        }
        return oss.str();
    }

private:
    std::string   name_;
    uint64_t      count_{0};
    uint64_t      sum_{0};
    double        sumSq_{0.0};
    int64_t       minNs_{INT64_MAX};
    int64_t       maxNs_{0};
    std::array<uint64_t, kBuckets> buckets_{};
    mutable std::mutex mu_;
    std::vector<uint64_t> samples_;
};

// ─────────────────────────────────────────────────────────────
// LatencyTimer — RAII scoped timer
// ─────────────────────────────────────────────────────────────

class LatencyTimer {
public:
    explicit LatencyTimer(LatencyStats& stats) noexcept
        : stats_{stats}, start_{hrnow()} {}

    ~LatencyTimer() noexcept {
        stats_.record(hrnow() - start_);
    }

    LatencyTimer(const LatencyTimer&)            = delete;
    LatencyTimer& operator=(const LatencyTimer&) = delete;

    [[nodiscard]] int64_t elapsedNs() const noexcept {
        return hrnow() - start_;
    }

private:
    LatencyStats& stats_;
    int64_t       start_;
};

// ─────────────────────────────────────────────────────────────
// LatencyBench — micro-benchmark harness
// ─────────────────────────────────────────────────────────────

class LatencyBench {
public:
    /**
     * @brief Run a benchmark: call fn() N times, record latency per call.
     *
     * @param name       Benchmark label
     * @param fn         The function to benchmark
     * @param iterations Number of iterations
     * @param warmup     Warmup iterations (not recorded)
     * @return LatencyStats with all samples
     */
    [[nodiscard]] static LatencyStats run(
            const std::string& name,
            std::function<void()> fn,
            uint64_t iterations = 100'000,
            uint64_t warmup     = 1'000)
    {
        LatencyStats stats{name};

        // Warmup: let CPU reach steady-state frequency
        for (uint64_t i = 0; i < warmup; ++i) fn();

        // Measure
        for (uint64_t i = 0; i < iterations; ++i) {
            int64_t t0 = hrnow();
            fn();
            int64_t elapsed = hrnow() - t0;
            stats.record(elapsed);
        }

        return stats;
    }

    /**
     * @brief Run multiple benchmarks and produce a comparison report.
     */
    struct BenchResult {
        std::string    name;
        LatencyStats   stats;
        double         throughputMps; ///< Million ops per second
    };

    [[nodiscard]] static std::vector<BenchResult> compare(
            std::vector<std::pair<std::string, std::function<void()>>> benches,
            uint64_t iterations = 100'000)
    {
        std::vector<BenchResult> results;
        for (auto& [name, fn] : benches) {
            auto stats = run(name, fn, iterations);
            double tps = (stats.meanNs() > 0)
                ? 1e3 / stats.meanNs()   // means → µs → Million ops/s
                : 0.0;
            results.push_back({name, std::move(stats), tps});
        }
        return results;
    }

    [[nodiscard]] static std::string compareReport(
            const std::vector<BenchResult>& results)
    {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2);
        oss << "╔══════════════════════════════════════════════════════════╗\n"
            << "║              LATENCY BENCHMARK REPORT                   ║\n"
            << "╠══════════════╦══════════╦══════════╦══════════╦═════════╣\n"
            << "║ Benchmark    ║  mean µs ║  p99 µs  ║  max µs  ║  M ops/s║\n"
            << "╠══════════════╬══════════╬══════════╬══════════╬═════════╣\n";
        for (auto& r : results) {
            oss << "║ " << std::setw(12) << std::left  << r.name.substr(0,12)
                << " ║ " << std::setw(8)  << std::right << r.stats.meanUs()
                << " ║ " << std::setw(8)  << r.stats.p99()/1000.0
                << " ║ " << std::setw(8)  << r.stats.maxUs()
                << " ║ " << std::setw(7)  << r.throughputMps << " ║\n";
        }
        oss << "╚══════════════╩══════════╩══════════╩══════════╩═════════╝\n";
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────
// ProfilingReport — aggregate profiling across the whole system
// ─────────────────────────────────────────────────────────────

class ProfilingReport {
public:
    void addStats(std::string category, LatencyStats stats) {
        sections_.push_back({std::move(category), std::move(stats)});
    }

    [[nodiscard]] std::string generate() const {
        std::ostringstream oss;
        oss << "\n╔══════════════════════════════════════════════╗\n"
            << "║     QuantTradingLab — Profiling Report       ║\n"
            << "╚══════════════════════════════════════════════╝\n\n";
        for (auto& [cat, stats] : sections_) {
            oss << "▶ " << cat << "\n";
            oss << stats.report() << "\n";
        }
        return oss.str();
    }

private:
    struct Section { std::string category; LatencyStats stats; };
    std::vector<Section> sections_;
};

} // namespace qtl
