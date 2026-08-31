// server/gateway/src/route/player_router.cpp — TASK-010 §15.3

#include "mmo/gateway/route/player_router.h"

#include <string>

namespace mmo::gateway {

core::Result<NodeId> PlayerRouter::Route(PlayerId player) {
    const std::uint64_t key = static_cast<std::uint64_t>(player);
    auto cached            = cache_.Get(key);
    if (cached.has_value()) {
        return core::Result<NodeId>::Ok(*cached);
    }
    // cache miss：走注册中心一致性哈希选 GameNode，回填缓存
    const auto picked = registry_.Pick(NodeRole::GameNode, std::to_string(player));
    if (!picked.HasValue()) {
        return core::Result<NodeId>::Fail(picked.Err());  // 全节点不可用时冒泡 BUSY
    }
    cache_.Put(key, picked.Value());
    return core::Result<NodeId>::Ok(picked.Value());
}

}  // namespace mmo::gateway
