#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "runi/core/result.hpp"

namespace runi {

struct ExecutorSnapshot {
    std::size_t workers{0};
    std::size_t queued{0};
    std::size_t active{0};
    std::size_t accepted{0};
    std::size_t rejected{0};
    bool accepting{false};
};

class BoundedExecutor {
public:
    BoundedExecutor(std::size_t worker_count, std::size_t queue_capacity);
    ~BoundedExecutor();

    BoundedExecutor(const BoundedExecutor&) = delete;
    BoundedExecutor& operator=(const BoundedExecutor&) = delete;
    BoundedExecutor(BoundedExecutor&&) = delete;
    BoundedExecutor& operator=(BoundedExecutor&&) = delete;

    template <typename Function>
    [[nodiscard]] auto submit(Function&& function)
        -> Result<std::future<std::invoke_result_t<std::decay_t<Function>&, std::stop_token>>> {
        using Return = std::invoke_result_t<std::decay_t<Function>&, std::stop_token>;
        auto task = std::make_shared<std::packaged_task<Return(std::stop_token)>>(std::forward<Function>(function));
        auto future = task->get_future();
        const auto queued = enqueue([task = std::move(task)](std::stop_token stop_token) {
            (*task)(stop_token);
        });
        if (!queued) return Result<std::future<Return>>::failure(queued.error());
        return Result<std::future<Return>>::success(std::move(future));
    }

    void request_stop() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] ExecutorSnapshot snapshot() const;

private:
    using Task = std::function<void(std::stop_token)>;

    [[nodiscard]] Result<void> enqueue(Task task);
    void worker_loop(std::stop_token stop_token) noexcept;

    const std::size_t queue_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<Task> queue_;
    std::vector<std::jthread> workers_;
    bool accepting_{true};
    std::atomic<bool> joined_{false};
    std::atomic<std::size_t> active_{0};
    std::atomic<std::size_t> accepted_{0};
    std::atomic<std::size_t> rejected_{0};
};

}  // namespace runi
