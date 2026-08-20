#include "runi/orchestration/execution.hpp"

#include <stdexcept>

namespace runi {

BoundedExecutor::BoundedExecutor(std::size_t worker_count, std::size_t queue_capacity)
    : queue_capacity_(queue_capacity) {
    if (worker_count == 0) throw std::invalid_argument("executor worker_count must be positive");
    if (queue_capacity == 0) throw std::invalid_argument("executor queue_capacity must be positive");
    workers_.reserve(worker_count);
    for (std::size_t index = 0; index < worker_count; ++index) {
        static_cast<void>(index);
        workers_.emplace_back([this](std::stop_token stop_token) { worker_loop(stop_token); });
    }
}

BoundedExecutor::~BoundedExecutor() {
    shutdown();
}

Result<void> BoundedExecutor::enqueue(Task task) {
    std::scoped_lock lock(mutex_);
    if (!accepting_) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return Result<void>::failure(make_error(
            ErrorCategory::Internal, "executor_stopped", "executor is not accepting new tasks"));
    }
    if (queue_.size() >= queue_capacity_) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return Result<void>::failure(make_error(
            ErrorCategory::Internal, "executor_queue_full", "executor queue capacity has been reached", true));
    }
    queue_.push_back(std::move(task));
    accepted_.fetch_add(1, std::memory_order_relaxed);
    condition_.notify_one();
    return Result<void>::success();
}

void BoundedExecutor::worker_loop(std::stop_token stop_token) noexcept {
    while (true) {
        Task task;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, stop_token, [this] { return !queue_.empty() || !accepting_; });
            if (queue_.empty()) {
                if (!accepting_ || stop_token.stop_requested()) return;
                continue;
            }
            task = std::move(queue_.front());
            queue_.pop_front();
            active_.fetch_add(1, std::memory_order_relaxed);
        }
        try {
            task(stop_token);
        } catch (...) {
            // Packaged tasks preserve user exceptions in their futures. This guard keeps
            // the worker alive if an internal task wrapper ever throws unexpectedly.
        }
        active_.fetch_sub(1, std::memory_order_relaxed);
    }
}

void BoundedExecutor::request_stop() noexcept {
    {
        std::scoped_lock lock(mutex_);
        accepting_ = false;
    }
    for (auto& worker : workers_) worker.request_stop();
    condition_.notify_all();
}

void BoundedExecutor::shutdown() noexcept {
    bool expected = false;
    if (!joined_.compare_exchange_strong(expected, true)) return;
    request_stop();
    workers_.clear();
}

ExecutorSnapshot BoundedExecutor::snapshot() const {
    std::scoped_lock lock(mutex_);
    return ExecutorSnapshot{
        workers_.size(), queue_.size(), active_.load(std::memory_order_relaxed),
        accepted_.load(std::memory_order_relaxed), rejected_.load(std::memory_order_relaxed), accepting_};
}

}  // namespace runi
