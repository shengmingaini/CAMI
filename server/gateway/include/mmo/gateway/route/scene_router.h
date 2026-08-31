// server/gateway/include/mmo/gateway/route/scene_router.h — TASK-010 §7 / §15.4
//
// SceneID → GameNode（Owner 唯一）：重复 Bind（不同节点）返回 VERSION_CONFLICT。
// 物理约束：同一 Scene 只能有一个权威 Owner（§4 / §6 / §21）。

#pragma once

#include <cstdint>

#include "mmo/core/error/result.h"
#include "mmo/gateway/route/node_registry.h"
#include "mmo/gateway/route/route_cache.h"
#include "mmo/gateway/session/session.h"

namespace mmo::gateway {

class SceneRouter {
public:
    SceneRouter(NodeRegistry& registry, RouteCache& cache)
        : registry_(registry), cache_(cache) {}

    /// 查询 Scene 的 Owner 节点；未绑定时返回 NOT_FOUND（调用方触发分配流程）。
    core::Result<NodeId> OwnerOf(SceneId scene);

    /// 绑定 Scene → Node。已绑定到**其他**节点返回 VERSION_CONFLICT（Owner 唯一性）。
    /// 重复绑定到**同一**节点返回 OK（幂等）。
    core::Result<void> Bind(SceneId scene, NodeId node);

    /// 解绑（节点迁移 / 场景销毁）。仅 Owner 本人可解绑，否则 VERSION_CONFLICT。
    core::Result<void> Unbind(SceneId scene, NodeId node);

    /// 节点 Dead 时批量失效该节点所有 Scene 归属（§6 / §19）。
    void InvalidateNode(NodeId node) { cache_.EraseByValue(node); }

    std::size_t CacheSize() const noexcept { return cache_.Size(); }

private:
    NodeRegistry& registry_;
    RouteCache&   cache_;
};

}  // namespace mmo::gateway
