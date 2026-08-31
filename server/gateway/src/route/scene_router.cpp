// server/gateway/src/route/scene_router.cpp — TASK-010 §15.4

#include "mmo/gateway/route/scene_router.h"

#include <string>

namespace mmo::gateway {
namespace {

using core::ErrorCode;

core::Error MakeErr(ErrorCode code, const char* msg) {
    return core::Error{code, msg, core::domain::kNet};
}

}  // namespace

core::Result<NodeId> SceneRouter::OwnerOf(SceneId scene) {
    const std::uint64_t key = static_cast<std::uint64_t>(scene);
    auto cached            = cache_.Get(key);
    if (cached.has_value()) {
        return core::Result<NodeId>::Ok(*cached);
    }
    // 未绑定：返回 NOT_FOUND，由调用方触发分配（§15.5 分配流程）
    return core::Result<NodeId>::Fail(
        MakeErr(ErrorCode::NOT_FOUND, "scene not bound"));
}

core::Result<void> SceneRouter::Bind(SceneId scene, NodeId node) {
    const std::uint64_t key = static_cast<std::uint64_t>(scene);
    auto cached            = cache_.Get(key);
    if (cached.has_value()) {
        if (*cached == node) {
            return core::Result<void>::Ok();  // 幂等：同一节点重复绑定
        }
        // 已绑定到其它节点：Owner 唯一性冲突
        return core::Result<void>::Fail(
            MakeErr(ErrorCode::VERSION_CONFLICT, "scene already owned by another node"));
    }
    // 校验目标节点存在且存活
    const auto alive = registry_.ListHealthy(NodeRole::GameNode);
    if (!alive.HasValue()) {
        return core::Result<void>::Fail(alive.Err());
    }
    bool found = false;
    for (const auto& n : alive.Value()) {
        if (n.id == node) {
            found = true;
            break;
        }
    }
    if (!found) {
        return core::Result<void>::Fail(
            MakeErr(ErrorCode::NOT_FOUND, "target node not alive"));
    }
    cache_.Put(key, node);
    return core::Result<void>::Ok();
}

core::Result<void> SceneRouter::Unbind(SceneId scene, NodeId node) {
    const std::uint64_t key = static_cast<std::uint64_t>(scene);
    auto cached            = cache_.Get(key);
    if (!cached.has_value()) {
        return core::Result<void>::Fail(MakeErr(ErrorCode::NOT_FOUND, "scene not bound"));
    }
    if (*cached != node) {
        return core::Result<void>::Fail(
            MakeErr(ErrorCode::VERSION_CONFLICT, "only owner can unbind"));
    }
    cache_.Erase(key);
    return core::Result<void>::Ok();
}

}  // namespace mmo::gateway
