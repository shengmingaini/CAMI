// server/gateway/src/route/gateway_router.cpp — TASK-010 §15.5 转发路径

#include "mmo/gateway/route/gateway_router.h"

#include <string>

namespace mmo::gateway {
namespace {

using core::ErrorCode;

core::Error MakeErr(ErrorCode code, const char* msg) {
    return core::Error{code, msg, core::domain::kNet};
}

}  // namespace

core::Result<NodeId> GatewayRouter::RouteUpstream(PlayerId player, SceneId scene) {
    // 1) 优先按 PlayerID 归属（cache miss 走注册中心一致性哈希选节点并回填）
    if (player != kInvalidPlayerId) {
        auto r = player_.Route(player);
        if (r.HasValue()) {
            return r;
        }
        // 全节点不可用：BUSY 冒泡（§19 / §20.5）
        if (r.Err().Code() == ErrorCode::BUSY) {
            return core::Result<NodeId>::Fail(r.Err());
        }
    }

    // 2) 未知玩家时按 SceneID 归属
    if (scene != kInvalidSceneId) {
        auto owned = scene_.OwnerOf(scene);
        if (owned.HasValue()) {
            return owned;  // 已绑定 → 直接路由
        }
        // 未绑定：首占者自动分配（一致性哈希选 GameNode 并 Bind），失败 NOT_FOUND
        auto picked = registry_.Pick(NodeRole::GameNode, std::to_string(scene));
        if (picked.HasValue()) {
            (void)scene_.Bind(scene, picked.Value());
            return picked;
        }
    }

    // 3) 都未知：返回 NOT_FOUND，触发上层分配流程（§15.5）
    return core::Result<NodeId>::Fail(MakeErr(ErrorCode::NOT_FOUND, "no route target"));
}

}  // namespace mmo::gateway
