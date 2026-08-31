#pragma once

/// TASK-012 · Scene ID 与玩家/节点 ID 类型（§7 / §15.1）。
///
/// 关键约束：SceneId **复用** TASK-011 实体模块的统一定义（uint64）。实体模块明确：
/// 「下游 Scene 模块接入后以同一定义为准（禁止重复定义第二套 SceneId）」。
/// 因此本文件**不重定义** SceneId 类型，只提供 SceneId 的「type + index」编码辅助，
/// 以及 Scene 模块自有、且不应依赖 gateway 的 PlayerId / NodeId / LeaveReason。

#include <cstdint>

#include "mmo/game/entity/entity_id.h"  // 复用 mmo::game::SceneId（uint64）

namespace mmo::game {

/// 玩家 ID（§7 SceneContext）。uint64，与网关会话域的 PlayerId 解耦（模块边界：
/// 本模块不依赖 gateway，禁止跨模块 include 其 src/ 或内部头）。
using PlayerId = std::uint64_t;

/// 拥有该 Scene 的 GameNode 节点 ID（§7 / §4 State Owner）。
using NodeId = std::uint32_t;

/// 玩家离开原因（§7 Leave）。
enum class LeaveReason : std::uint8_t {
    Disconnect = 0,   // 连接断开
    Logout = 1,       // 主动登出
    Kick = 2,         // 被踢
    Transfer = 3,     // 场景切换（跨 Scene 转移）
    Timeout = 4,      // 心跳超时
};

/// 由场景类型（高 8 位）与实例序号（低 56 位）合成全局唯一 SceneId（§15.1）。
/// 与实体模块的 SceneId(uint64) 同类型，禁止重定义第二套。
inline constexpr SceneId MakeSceneId(std::uint8_t type, std::uint64_t index) noexcept {
    return (static_cast<SceneId>(type) << 56) |
           (index & 0x00FFFFFFFFFFFFFFull);
}

inline constexpr std::uint8_t SceneTypeOf(SceneId id) noexcept {
    return static_cast<std::uint8_t>(id >> 56);
}

inline constexpr std::uint64_t SceneIndexOf(SceneId id) noexcept {
    return id & 0x00FFFFFFFFFFFFFFull;
}

}  // namespace mmo::game
