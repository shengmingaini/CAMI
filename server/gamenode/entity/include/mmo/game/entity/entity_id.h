#pragma once

/// TASK-011 · EntityId 编码与 SceneId 类型（§7 / §8）。
///
/// EntityId = [generation:32][index:32]（低位 32 是 SlotMap 槽位索引，高位 32 是世代号）。
/// 槽位回收时世代 +1，使旧 EntityId 查找必然返回 nullptr，杜绝 ABA 悬垂（§8 / §20.1）。

#include <cstdint>

namespace mmo::game {

/// 全局唯一实体 ID（§7）。销毁后旧 ID 不可复用（世代机制保证）。
using EntityId = std::uint64_t;

/// 场景 ID。本任务协议层（TASK-005）尚未落地 SceneId，实体模块自有其接口参数类型；
/// 下游 Scene 模块接入后以同一定义为准（禁止重复定义第二套 SceneId）。
using SceneId = std::uint64_t;

/// 无效场景（约定 0 表示「未绑定场景」，与协议层 kInvalidSceneId 语义一致）。
inline constexpr SceneId kInvalidSceneId = 0;

/// 由槽位索引与世代号合成 EntityId（§8）。
inline constexpr EntityId MakeEntityId(std::uint32_t index,
                                       std::uint32_t generation) noexcept {
    return (static_cast<EntityId>(index) << 32) |
           static_cast<EntityId>(generation);
}

/// 取 EntityId 的高 32 位：SlotMap 槽位索引（§7 编码 (index<<32)|generation）。
inline constexpr std::uint32_t EntityIndex(EntityId id) noexcept {
    return static_cast<std::uint32_t>(id >> 32);
}

/// 取 EntityId 的低 32 位：世代号（防 ABA）。
inline constexpr std::uint32_t EntityGeneration(EntityId id) noexcept {
    return static_cast<std::uint32_t>(id & 0xFFFFFFFFu);
}

}  // namespace mmo::game
