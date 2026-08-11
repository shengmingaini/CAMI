// ============================================================================
// data/redis_proxy/cache_proxy.cpp — CacheProxy 实现 (Week4 D3)
// [PRODUCTION] 缓存策略逻辑 (与具体后端无关, 可接 Redis/内存)
// ============================================================================
#include "data/redis_proxy/cache_proxy.h"

#include <iostream>

namespace cami {
namespace data {
namespace redis_proxy {

std::optional<std::string> CacheProxy::Get(std::string_view key) {
    // 1) 缓存命中
    if (auto v = backend_.Get(key)) {
        std::lock_guard<std::mutex> lk(stats_mu_);
        hits_ += 1;
        if (enable_hot_) hot_.record(key);
        return v;
    }
    // 2) 缓存未命中 -> 回源 (Read-Through)
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        misses_ += 1;
    }
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
        // 写回: 标记 dirty, 由 FlushDirty / D4 sync 模块定时批量落库
        std::lock_guard<std::mutex> lk(dirty_mu_);
        dirty_.emplace(std::string(key));
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
    std::unordered_set<std::string> batch;
    {
        std::lock_guard<std::mutex> lk(dirty_mu_);
        batch.swap(dirty_);
    }
    if (batch.empty()) return 0;

    // 已注入异步落库 sink (生产: Kafka 发布) -> 走异步路径
    if (async_flush_sink_) {
        std::vector<std::pair<std::string, std::string>> pairs;
        pairs.reserve(batch.size());
        for (const auto& k : batch) {
            auto v = backend_.Get(k);  // 取当前缓存值
            if (v) {
                pairs.emplace_back(k, std::move(*v));
            } else {
                // 缓存被驱逐/过期: WriteBack 单副本语义下 value 已丢, 重试永远取不到 ->
                // 告警并放弃 (否则每次 FlushDirty 都重建该 key, dirty 无限累积死循环)。
                std::cerr << "[CacheProxy] dirty key evicted before flush (value lost), drop: "
                          << k << std::endl;
            }
        }
        bool all_ok = async_flush_sink_(pairs);
        if (!all_ok) {  // 部分/全部发布失败 -> 已取出 key 重新标记 dirty, 下次重试 (背压)
            std::lock_guard<std::mutex> lk(dirty_mu_);
            for (auto& kv : pairs) dirty_.insert(kv.first);
        }
        return pairs.size();
    }

    // 回退: 同步 STORE (OFF/demo 默认, 保证单测/仿真向后兼容)
    for (auto& k : batch) {
        auto v = backend_.Get(k);
        if (v) store_.Store(k, *v);
    }
    return batch.size();
}

}  // namespace redis_proxy
}  // namespace data
}  // namespace cami
