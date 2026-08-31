// server/gateway/src/route/route_cache.cpp — TASK-010 §15.2

#include "mmo/gateway/route/route_cache.h"

#include <algorithm>
#include <utility>

namespace mmo::gateway {

RouteCache::RouteCache(std::size_t capacity)
    : capacity_per_shard_(std::max<std::size_t>(1, capacity / kShards)) {
    shards_.reserve(kShards);
    for (std::size_t i = 0; i < kShards; ++i) {
        auto s = std::make_unique<Shard>();
        s->capacity = capacity_per_shard_;
        shards_.push_back(std::move(s));
    }
}

std::optional<NodeId> RouteCache::Get(std::uint64_t key) const {
    const Shard& s = *shards_[ShardOf(key)];
    std::lock_guard<std::mutex> lock(s.mu);
    auto it = s.index.find(key);
    if (it == s.index.end()) {
        ++s.misses;
        return std::nullopt;
    }
    // 命中：移到 MRU 头（热路径 O(1) splice）
    s.lru.splice(s.lru.begin(), s.lru, it->second);
    ++s.hits;
    return it->second->second;
}

void RouteCache::Put(std::uint64_t key, NodeId value) {
    Shard& s = *shards_[ShardOf(key)];
    std::lock_guard<std::mutex> lock(s.mu);
    auto it = s.index.find(key);
    if (it != s.index.end()) {
        it->second->second = value;               // 更新 value，保持 MRU
        s.lru.splice(s.lru.begin(), s.lru, it->second);
        return;
    }
    // 淘汰该片 LRU 末尾（冷项）
    if (s.index.size() >= s.capacity && !s.lru.empty()) {
        const auto& victim = s.lru.back();
        s.index.erase(victim.first);
        s.lru.pop_back();
        ++evictions_;
    }
    s.lru.emplace_front(key, value);
    s.index.emplace(key, s.lru.begin());
}

void RouteCache::Erase(std::uint64_t key) {
    Shard& s = *shards_[ShardOf(key)];
    std::lock_guard<std::mutex> lock(s.mu);
    auto it = s.index.find(key);
    if (it == s.index.end()) {
        return;
    }
    s.lru.erase(it->second);
    s.index.erase(it);
}

void RouteCache::EraseByValue(NodeId value) {
    for (std::size_t i = 0; i < kShards; ++i) {
        Shard& s = *shards_[i];
        std::lock_guard<std::mutex> lock(s.mu);
        for (auto it = s.lru.begin(); it != s.lru.end();) {
            if (it->second == value) {
                s.index.erase(it->first);
                it = s.lru.erase(it);
            } else {
                ++it;
            }
        }
    }
}

std::size_t RouteCache::Size() const noexcept {
    std::size_t total = 0;
    for (const auto& s : shards_) {
        std::lock_guard<std::mutex> lock(s->mu);
        total += s->index.size();
    }
    return total;
}

double RouteCache::HitRate() const noexcept {
    std::uint64_t hits = 0, misses = 0;
    for (const auto& s : shards_) {
        std::lock_guard<std::mutex> lock(s->mu);
        hits += s->hits;
        misses += s->misses;
    }
    const std::uint64_t total = hits + misses;
    if (total == 0) {
        return 0.0;
    }
    return static_cast<double>(hits) / static_cast<double>(total);
}

void RouteCache::ResetStats() noexcept {
    for (const auto& s : shards_) {
        std::lock_guard<std::mutex> lock(s->mu);
        s->hits = 0;
        s->misses = 0;
    }
}

}  // namespace mmo::gateway
