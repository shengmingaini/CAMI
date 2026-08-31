// server/gateway/include/mmo/gateway/route/player_router.h — TASK-010 §7 / §15.3
//
// PlayerID → GameNode 路由：cache miss 走 NodeRegistry 一致性哈希选节点并回填。
// 命中率指标目标 > 99%（§20.3 / §22）。

#pragma once

#include <cstdint>

#include "mmo/core/error/result.h"
#include "mmo/gateway/route/node_registry.h"
#include "mmo/gateway/route/route_cache.h"
#include "mmo/gateway/session/session.h"

namespace mmo::gateway {

class PlayerRouter {
public:
    PlayerRouter(NodeRegistry& registry, RouteCache& cache)
        : registry_(registry), cache_(cache) {}

    /// 查询玩家归属节点：命中直接返回；未命中走注册中心 Pick(GameNode) 并回填缓存。
    core::Result<NodeId> Route(PlayerId player);

    /// 失效单个玩家缓存（玩家下线 / 迁移时）。
    void Invalidate(PlayerId player) { cache_.Erase(static_cast<std::uint64_t>(player)); }

    /// 节点 Dead 时批量失效该节点所有条目（§6 / §19）。
    void InvalidateNode(NodeId node) { cache_.EraseByValue(node); }

    /// 命中率（0..1），目标 > 0.99。
    double CacheHitRate() const noexcept { return cache_.HitRate(); }

    std::size_t CacheSize() const noexcept { return cache_.Size(); }

private:
    NodeRegistry& registry_;
    RouteCache&   cache_;
};

}  // namespace mmo::gateway
