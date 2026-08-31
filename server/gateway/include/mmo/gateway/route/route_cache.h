// server/gateway/include/mmo/gateway/route/route_cache.h — TASK-010 §7 / §8 / §15.2
//
// 有界 LRU 路由缓存：16 分片，每片独立锁（**禁止全局锁**，§21 / §9）。
// 节点失效时按 value（NodeId）批量失效（§6 / §19）。

#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "mmo/gateway/session/session.h"

namespace mmo::gateway {

class RouteCache {
public:
    static constexpr std::size_t kShards = 16;
    static constexpr std::size_t kDefaultCapacity = 100'000;

    explicit RouteCache(std::size_t capacity = kDefaultCapacity);

    /// 查询；命中返回 NodeId，未命中返回 nullopt（调用方走注册中心回填）。
    std::optional<NodeId> Get(std::uint64_t key) const;

    /// 写入 / 更新；分片满则淘汰该片 LRU 末尾（冷项）。
    void Put(std::uint64_t key, NodeId value);

    /// 精确删除某个 key。
    void Erase(std::uint64_t key);

    /// 按 value 批量失效（节点 Dead 时调用）：删除所有命中该 NodeId 的条目。
    void EraseByValue(NodeId value);

    std::size_t Size() const noexcept;

    /// 命中率 = hits / (hits + misses)。无查询返回 0。
    double HitRate() const noexcept;

    /// 清零命中/未命中计数（供 benchmark 在进入测量窗口前重置，得到稳态命中率）。
    /// 不触碰缓存数据，仅清观测计数。
    void ResetStats() noexcept;

    /// 累计淘汰次数（供测试 / 观测）。
    std::size_t Evictions() const noexcept { return evictions_; }

private:
    struct Shard {
        mutable std::mutex    mu;
        // 以下字段受 mu 保护：Get() 虽标 const，但需更新 LRU 序与命中计数
        // （逻辑 const —— 对外取值不变，内部簿记可变），故标 mutable。
        mutable std::list<std::pair<std::uint64_t, NodeId>> lru;  // front=MRU, back=LRU
        std::unordered_map<std::uint64_t, std::list<std::pair<std::uint64_t, NodeId>>::iterator> index;
        std::size_t capacity{0};
        mutable std::uint64_t hits{0};
        mutable std::uint64_t misses{0};
    };

    static std::size_t ShardOf(std::uint64_t key) noexcept {
        return key & (kShards - 1);
    }

    std::size_t                         capacity_per_shard_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::uint64_t                       evictions_{0};
};

}  // namespace mmo::gateway
