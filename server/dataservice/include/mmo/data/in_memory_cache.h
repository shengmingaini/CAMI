// server/dataservice/include/mmo/data/in_memory_cache.h
//
// InMemoryCache —— ICache 的内存实现（TASK-026）。
//
// 有界 LRU（容量上限）+ TTL 过期。Put 更新 LRU 位置；超出容量时淘汰最久未用；
// Get 命中时若已过期则视为未命中并剔除。方法体见 src/in_memory_cache.cpp。

#pragma once

#include <cstdint>
#include <list>
#include <string>
#include <unordered_map>

#include "mmo/core/time/clock.h"
#include "mmo/data/icache.h"
#include "mmo/data/record.h"

namespace mmo::data {

/// ICache 的内存实现（易失缓存的本地替身）。
class InMemoryCache final : public ICache {
public:
    explicit InMemoryCache(std::size_t capacity = 1024) : capacity_(capacity) {}

    /// 缓存条目（公开以便测试与实现共享；含 LRU 迭代器与 TTL 状态）。
    struct Entry {
        Record rec;
        mmo::core::SteadyTime expire_at{};   // 过期时刻；has_ttl=false 时忽略
        bool has_ttl{false};
        std::list<DataKey>::iterator lru_it{};  // LRU 位置（O(1) 提升/淘汰）
    };

    core::Result<std::optional<Record>> Get(const DataKey& key) override;
    core::Result<void> Put(const Record& rec, mmo::core::DurationMs ttl = {}) override;
    core::Result<void> Invalidate(const DataKey& key) override;
    core::Result<void> InvalidatePrefix(std::string_view prefix) override;

    /// 命中率（命中 / (命中 + 未命中)）。
    double hit_rate() const noexcept;
    std::uint64_t hits() const noexcept { return hits_; }
    std::uint64_t misses() const noexcept { return misses_; }
    std::uint64_t evictions() const noexcept { return evictions_; }
    std::size_t size() const noexcept { return map_.size(); }
    std::size_t capacity() const noexcept { return capacity_; }

private:
    std::size_t capacity_;
    std::unordered_map<DataKey, Entry> map_;
    std::list<DataKey> lru_;                  // 队首 = 最近使用
    std::uint64_t hits_{0};
    std::uint64_t misses_{0};
    std::uint64_t evictions_{0};
};

}  // namespace mmo::data
