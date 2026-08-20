#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <future>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

#include "runi/core/result.hpp"

namespace runi {

struct ContextCacheSnapshot {
    std::size_t size{0};
    std::size_t hits{0};
    std::size_t misses{0};
    std::size_t evictions{0};
    std::size_t coalesced{0};
};

class ContextCache {
public:
    explicit ContextCache(std::size_t capacity);

    [[nodiscard]] Result<std::string> get_or_compute(
        std::string key,
        std::chrono::milliseconds ttl,
        const std::function<Result<std::string>()>& compute);
    void invalidate(std::string_view key);
    void clear();
    [[nodiscard]] ContextCacheSnapshot snapshot() const;

private:
    using Clock = std::chrono::steady_clock;
    struct Entry {
        std::string value;
        Clock::time_point expires_at;
        std::list<std::string>::iterator lru;
    };

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::list<std::string> lru_;
    std::map<std::string, Entry, std::less<>> entries_;
    std::map<std::string, std::shared_future<Result<std::string>>, std::less<>> inflight_;
    std::size_t hits_{0};
    std::size_t misses_{0};
    std::size_t evictions_{0};
    std::size_t coalesced_{0};
};

}  // namespace runi
