// server/gateway/include/mmo/gateway/route/gateway_router.h — TASK-010 §15.5/§15.6/§15.7
//
// Gateway 路由门面：把 NodeRegistry + RouteCache + PlayerRouter + SceneRouter 装配成
// 一条可测试的转发路径。属本任务的「扩展装配点」（§7 列出了四个组成类，本门面是它们的
// 接线器，不新增对外契约语义）。
//
// 转发优先级：PlayerID 归属 → SceneID 归属 → 全未知返回 NOT_FOUND 触发分配流程。
// 节点失效：Registry 在 Tick 中判 Dead 并发布 NodeDead；本门面订阅该事件，批量失效缓存。

#pragma once

#include <string>
#include <string_view>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/core/time/clock.h"
#include "mmo/gateway/route/node_registry.h"
#include "mmo/gateway/route/player_router.h"
#include "mmo/gateway/route/route_cache.h"
#include "mmo/gateway/route/scene_router.h"
#include "mmo/gateway/session/session.h"

namespace mmo::gateway {

class GatewayRouter {
public:
    explicit GatewayRouter(core::EventBus* bus = nullptr,
                            std::size_t cache_capacity = RouteCache::kDefaultCapacity,
                            NodeRegistry::Config reg_cfg = {})
        : bus_(bus),
          cache_(cache_capacity),
          registry_(bus, reg_cfg),
          player_(registry_, cache_),
          scene_(registry_, cache_) {
        if (bus_ != nullptr) {
            (void)bus_->Subscribe<NodeDead>([this](const NodeDead& e) {
                this->OnNodeDead(e.node_id);
            });
        }
    }

    // ---- 节点管理（透传 NodeRegistry）----
    core::Result<void> RegisterNode(const NodeInfo& info) { return registry_.Register(info); }
    core::Result<void> Heartbeat(NodeId id, std::uint32_t load) {
        return registry_.Heartbeat(id, load);
    }
    core::Result<void> UnregisterNode(NodeId id) { return registry_.Unregister(id); }

    // ---- 转发（§15.5）----
    /// 上行包路由：先按 PlayerID，未知则按 SceneID（未绑定自动分配并绑定），
    /// 都未知返回 NOT_FOUND（触发上层分配流程）。无存活节点冒泡 BUSY。
    core::Result<NodeId> RouteUpstream(PlayerId player, SceneId scene);

    /// 显式绑定场景（分配流程调用）：返回 VERSION_CONFLICT 表示已有 Owner。
    core::Result<void> BindScene(SceneId scene, NodeId node) {
        return scene_.Bind(scene, node);
    }

    // ---- 节点失效（§15.6 / §19）----
    /// 驱动注册中心心跳超时扫描；Dead 节点由 NodeDead 订阅回调批量失效缓存。
    core::Result<void> Tick(core::SteadyTime now) { return registry_.Tick(now); }

    void OnNodeDead(NodeId node) {
        player_.InvalidateNode(node);
        scene_.InvalidateNode(node);
    }

    // ---- 观测 ----
    double        CacheHitRate() const noexcept { return cache_.HitRate(); }
    std::size_t   CacheSize() const noexcept { return cache_.Size(); }
    std::size_t   HealthyCount(NodeRole role) const noexcept { return registry_.HealthyCount(role); }

    // 组件访问（测试 / 宿主装配用）
    NodeRegistry& registry() noexcept { return registry_; }
    RouteCache&    cache() noexcept { return cache_; }
    PlayerRouter&  player_router() noexcept { return player_; }
    SceneRouter&   scene_router() noexcept { return scene_; }

private:
    core::EventBus* bus_;
    RouteCache      cache_;
    NodeRegistry    registry_;
    PlayerRouter    player_;
    SceneRouter     scene_;
};

}  // namespace mmo::gateway
