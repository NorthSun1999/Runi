#include "runi/state/context_cache.hpp"

#include <exception>
#include <stdexcept>

namespace runi {

ContextCache::ContextCache(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) throw std::invalid_argument("context cache capacity must be positive");
}

Result<std::string> ContextCache::get_or_compute(
    std::string key,
    std::chrono::milliseconds ttl,
    const std::function<Result<std::string>()>& compute) {
    std::shared_future<Result<std::string>> shared;
    std::shared_ptr<std::promise<Result<std::string>>> promise;
    bool leader = false;
    {
        std::scoped_lock lock(mutex_);
        const auto now = Clock::now();
        auto existing = entries_.find(key);
        if (existing != entries_.end() && existing->second.expires_at > now) {
            lru_.splice(lru_.begin(), lru_, existing->second.lru);
            ++hits_;
            return Result<std::string>::success(existing->second.value);
        }
        if (existing != entries_.end()) {
            lru_.erase(existing->second.lru);
            entries_.erase(existing);
        }
        const auto pending = inflight_.find(key);
        if (pending != inflight_.end()) {
            shared = pending->second;
            ++coalesced_;
        } else {
            promise = std::make_shared<std::promise<Result<std::string>>>();
            shared = promise->get_future().share();
            inflight_.emplace(key, shared);
            ++misses_;
            leader = true;
        }
    }

    if (!leader) return shared.get();

    Result<std::string> result = Result<std::string>::failure(make_error(
        ErrorCategory::Internal, "cache_compute_failed", "Context cache computation did not run"));
    try {
        result = compute();
    } catch (const std::exception& error) {
        result = Result<std::string>::failure(make_error(
            ErrorCategory::Internal, "cache_compute_exception", error.what()));
    } catch (...) {
        result = Result<std::string>::failure(make_error(
            ErrorCategory::Internal, "cache_compute_exception", "Unknown cache computation exception"));
    }

    {
        std::scoped_lock lock(mutex_);
        inflight_.erase(key);
        if (result) {
            const auto existing = entries_.find(key);
            if (existing != entries_.end()) {
                lru_.erase(existing->second.lru);
                entries_.erase(existing);
            }
            lru_.push_front(key);
            entries_.emplace(key, Entry{result.value(), Clock::now() + ttl, lru_.begin()});
            while (entries_.size() > capacity_) {
                const auto victim = lru_.back();
                lru_.pop_back();
                entries_.erase(victim);
                ++evictions_;
            }
        }
    }
    promise->set_value(result);
    return result;
}

void ContextCache::invalidate(std::string_view key) {
    std::scoped_lock lock(mutex_);
    const auto existing = entries_.find(key);
    if (existing == entries_.end()) return;
    lru_.erase(existing->second.lru);
    entries_.erase(existing);
}

void ContextCache::clear() {
    std::scoped_lock lock(mutex_);
    entries_.clear();
    lru_.clear();
}

ContextCacheSnapshot ContextCache::snapshot() const {
    std::scoped_lock lock(mutex_);
    return ContextCacheSnapshot{entries_.size(), hits_, misses_, evictions_, coalesced_};
}

}  // namespace runi
