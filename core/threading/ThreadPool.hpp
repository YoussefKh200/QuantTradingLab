#pragma once
/**
 * @file core/threading/ThreadPool.hpp
 * @brief Fixed-size thread pool for parallel analytics / batch tasks.
 *
 * Used for:
 *  - Monte Carlo simulation workers
 *  - Parameter sweep jobs
 *  - Parallel Greeks calculation over options chains
 *
 * NOT used on the HFT hot path (which is single-threaded / SPSC).
 */

#include <thread>
#include <vector>
#include <queue>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <future>
#include <stdexcept>
#include <atomic>
#include <type_traits>

namespace qtl {

class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads) {
        if (numThreads == 0)
            throw std::invalid_argument("ThreadPool: numThreads must be > 0");

        workers_.reserve(numThreads);
        for (size_t i = 0; i < numThreads; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard lock{mutex_};
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& w : workers_) {
            if (w.joinable()) w.join();
        }
    }

    // Non-copyable, non-movable
    ThreadPool(const ThreadPool&)            = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /**
     * @brief Submit a callable and return a future for its result.
     *
     * Example:
     * @code
     *   auto fut = pool.submit([](){ return computeGreeks(...); });
     *   auto result = fut.get();
     * @endcode
     */
    template<typename F, typename... Args>
    [[nodiscard]] auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using RetT = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<RetT()>>(
            [func = std::forward<F>(f),
             ...capturedArgs = std::forward<Args>(args)]() mutable {
                return func(std::forward<Args>(capturedArgs)...);
            });

        std::future<RetT> fut = task->get_future();
        {
            std::lock_guard lock{mutex_};
            if (stop_)
                throw std::runtime_error("ThreadPool: submit on stopped pool");
            tasks_.emplace([t = std::move(task)]{ (*t)(); });
        }
        cv_.notify_one();
        return fut;
    }

    /// Wait until all tasks currently queued have completed.
    void waitAll() {
        std::unique_lock lock{mutex_};
        cvDone_.wait(lock, [this]{
            return tasks_.empty() && activeCount_.load() == 0;
        });
    }

    [[nodiscard]] size_t threadCount()  const noexcept { return workers_.size(); }
    [[nodiscard]] size_t pendingTasks() const noexcept {
        std::lock_guard lock{mutex_};
        return tasks_.size();
    }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock{mutex_};
                cv_.wait(lock, [this]{
                    return !tasks_.empty() || stop_;
                });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
                ++activeCount_;
            }
            task();
            {
                std::lock_guard lock{mutex_};
                --activeCount_;
            }
            cvDone_.notify_all();
        }
    }

    std::vector<std::thread>           workers_;
    std::queue<std::function<void()>>  tasks_;
    mutable std::mutex                 mutex_;
    std::condition_variable            cv_;
    std::condition_variable            cvDone_;
    std::atomic<int>                   activeCount_{0};
    bool                               stop_{false};
};

} // namespace qtl
