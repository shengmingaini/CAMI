// ============================================================================
// data/redis_proxy/cache_proxy.cpp — CacheProxy 实现 (Week4 D3 / 2026-08-12 优化)
// [PRODUCTION] 缓存策略逻辑 (与具体后端无关, 可接 Redis/内存)
// [2026-08-12] 热路径优化: stats 原子计数(无锁) / dirty 缓冲存值(免回读) /
//              热点采样降频 / 读命中不搬 LRU (见 gateway 层同类审查)。
// ============================================================================
#include "data/redis_proxy/cache_proxy.h"

#include <iostream>

namespace cami {
namespace data {
namespace redis_proxy {

std::optional<std::string> CacheProxy::Get(std::string_view key) {
    // 1) 缓存命中 (统计为原子无锁自增)
    if (auto v = backend_.Get(key)) {
        hits_.fetch_add(1, std::memory_order_relaxed);
        if (enable_hot_) hot_.record(key);
        return v;
    }
    // 2) 缓存未命中 -> 回源 (Read-Through)
    misses_.fetch_add(1, std::memory_order_relaxed);
    auto from_db = store_.Load(key);
    if (enable_hot_) hot_.record(key);
    if (!from_db) return std::nullopt;  // 数据不存在
    // 3) 回填缓存
    backend_.Put(key, *from_db);
    return from_db;
}

void CacheProxy::Put(std::string_view key, std::string value, WritePolicy policy) {
    WritePolicy p = (policy == kDefault) ? policy_ : policy;
    // 始终先落缓存 (低延迟读写路径)
    backend_.Put(key, value);
    if (enable_hot_) hot_.record(key);

    if (p == WritePolicy::WriteThrough) {
        // 直写: 同步落 DB (强一致, 写放大但无丢失风险)
        store_.Store(key, value);
    } else {
        // 写回: 脏缓冲存 {key, value} (FlushDirty 免回读缓存; 缓存驱逐不丢值)
        std::lock_guard<std::mutex> lk(dirty_mu_);
        dirty_[std::string(key)] = std::move(value);
    }
}

void CacheProxy::Delete(std::string_view key) {
    backend_.Delete(key);
    {
        std::lock_guard<std::mutex> lk(dirty_mu_);
        dirty_.erase(std::string(key));
    }
    // 回源删除 (缓存与 DB 双删, 避免脏读); BackingStore::Delete 删行 (方案 B, 替代原 Store(key,"") hack)
    store_.Delete(key);
}

std::size_t CacheProxy::FlushDirty() {
    std::unordered_map<std::string, std::string> batch;
    {
        std::lock_guard<std::mutex> lk(dirty_mu_);
        batch.swap(dirty_);  // 整体出队 (O(1))
    }
    if (batch.empty()) return 0;

    // 已注入异步落库 sink (生产: Kafka 发布) -> 走异步路径
    if (async_flush_sink_) {
        std::vector<std::pair<std::string, std::string>> pairs;
        pairs.reserve(batch.size());
        for (auto& kv : batch) {
            pairs.emplace_back(kv.first, std::move(kv.second));  // 值来自脏缓冲, 免回读缓存
        }
        bool all_ok = async_flush_sink_(pairs);
        if (!all_ok) {  // 部分/全部发布失败 -> 重新入队重试 (背压); emplace 不覆盖更新的新值
            std::lock_guard<std::mutex> lk(dirty_mu_);
            for (auto& kv : pairs) dirty_.emplace(kv.first, kv.second);
        }
        return pairs.size();
    }

    // 回退: 同步 STORE (OFF/demo 默认, 保证单测/仿真向后兼容)
    for (auto& kv : batch) {
        store_.Store(kv.first, kv.second);
    }
    return batch.size();
}

}  // namespace redis_proxy
}  // namespace data
}  // namespace cami
