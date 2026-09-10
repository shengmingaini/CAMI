// server/dataservice/src/in_memory_cache.cpp
//
// InMemoryCache 方法体（TASK-026）。有界 LRU + TTL 过期。

#include "mmo/data/in_memory_cache.h"

#include <chrono>

namespace mmo::data {

namespace {
inline bool expired(const InMemoryCache::Entry& e, mmo::core::SteadyTime now) {
    return e.has_ttl && now > e.expire_at;
}
}  // namespace

core::Result<std::optional<Record>> InMemoryCache::Get(const DataKey& key) {
    auto it = map_.find(key);
    if (it == map_.end()) {
        ++misses_;
        return core::Result<std::optional<Record>>::Ok(std::nullopt);
    }
    const mmo::core::SteadyTime now = std::chrono::steady_clock::now();
    if (expired(it->second, now)) {
        lru_.erase(it->second.lru_it);
        map_.erase(it);
        ++misses_;
        return core::Result<std::optional<Record>>::Ok(std::nullopt);
    }
    // O(1) LRU 提升：把本键移到队首。
    lru_.splice(lru_.begin(), lru_, it->second.lru_it);
    ++hits_;
    return core::Result<std::optional<Record>>::Ok(it->second.rec);
}

core::Result<void> InMemoryCache::Put(const Record& rec, mmo::core::DurationMs ttl) {
    auto it = map_.find(rec.key);
    const mmo::core::SteadyTime now = std::chrono::steady_clock::now();
    if (it == map_.end()) {
        Entry e;
        e.rec = rec;
        e.has_ttl = (ttl > mmo::core::DurationMs{0});
        if (e.has_ttl) e.expire_at = now + ttl;
        lru_.push_front(rec.key);
        e.lru_it = lru_.begin();
        map_.emplace(rec.key, std::move(e));
    } else {
        it->second.rec = rec;
        it->second.has_ttl = (ttl > mmo::core::DurationMs{0});
        if (it->second.has_ttl) it->second.expire_at = now + ttl;
        lru_.splice(lru_.begin(), lru_, it->second.lru_it);
    }
    // 容量背压：超出则淘汰最久未用。
    while (map_.size() > capacity_) {
        const DataKey victim = lru_.back();
        auto vit = map_.find(victim);
        if (vit != map_.end()) map_.erase(vit);
        lru_.pop_back();
        ++evictions_;
    }
    return core::Result<void>::Ok();
}

core::Result<void> InMemoryCache::Invalidate(const DataKey& key) {
    auto it = map_.find(key);
    if (it != map_.end()) {
        lru_.erase(it->second.lru_it);
        map_.erase(it);
    }
    return core::Result<void>::Ok();
}

core::Result<void> InMemoryCache::InvalidatePrefix(std::string_view prefix) {
    if (prefix.empty()) return core::Result<void>::Ok();
    for (auto it = map_.begin(); it != map_.end();) {
        if (it->first.size() >= prefix.size() &&
            it->first.compare(0, prefix.size(), prefix) == 0) {
            lru_.erase(it->second.lru_it);
            it = map_.erase(it);
        } else {
            ++it;
        }
    }
    return core::Result<void>::Ok();
}

double InMemoryCache::hit_rate() const noexcept {
    const std::uint64_t total = hits_ + misses_;
    return total == 0 ? 0.0 : static_cast<double>(hits_) / static_cast<double>(total);
}

}  // namespace mmo::data
