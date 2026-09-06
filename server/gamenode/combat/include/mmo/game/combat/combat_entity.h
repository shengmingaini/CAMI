#pragma once

/// TASK-024 · 战斗实体状态（§7 / §8 / §15.2）。
///
/// 战斗热路径（§10 / §21 Forbidden）：禁止外部存储 / 缓存 / 消息中间件 / 同步远程调用 / 文件 IO /
/// 堆分配 / 线程 / 跨进程调用。状态全部本地内存，仇恨表定长内嵌（禁止无界增长）。

#include <cstdint>

#include "mmo/core/time/clock.h"
#include "mmo/game/combat/threat_table.h"
#include "mmo/game/entity/entity_id.h"

namespace mmo::game::combat {

/// 战斗状态位标记（§8 / §21 Forbidden：禁止散落 bool 成员表示战斗状态）。
enum class CombatFlag : std::uint32_t {
    None = 0,
    InCombat = 1u << 0,    // 处于战斗中
    Casting = 1u << 1,     // 读条中
    Stunned = 1u << 2,     // 被眩晕（来自控制类 Buff）
    Rooted = 1u << 3,      // 被定身（来自控制类 Buff）
    Silenced = 1u << 4,    // 被沉默（来自控制类 Buff）
    Invulnerable = 1u << 5,// 无敌（来自控制类 Buff）
    Dead = 1u << 6,        // 已死亡
};

/// 位标记底层类型（兼容任务书 §7 `CombatComponent::Flags` 写法）。
struct CombatComponent {
    using Flags = std::uint32_t;
};

/// 设置 / 清除 / 测试位标记（避免 enum operator 重载歧义）。
inline void SetFlag(CombatComponent::Flags& f, CombatFlag flag) noexcept {
    f |= static_cast<CombatComponent::Flags>(flag);
}
inline void ClearFlag(CombatComponent::Flags& f, CombatFlag flag) noexcept {
    f &= ~static_cast<CombatComponent::Flags>(flag);
}
inline bool TestFlag(CombatComponent::Flags f, CombatFlag flag) noexcept {
    return (f & static_cast<CombatComponent::Flags>(flag)) != 0;
}

/// 单实体战斗状态（§7）。仇恨表定长内嵌（§15.2 禁止无界增长）。
struct CombatEntity {
    EntityId id{0};
    CombatComponent::Flags flags{0};
    EntityId target{0};
    ThreatTable threat;
    core::SteadyTime last_combat_at{};  // 最近一次战斗行为时刻（脱战计时基准）
    std::uint32_t version{0};           // 状态版本（乐观并发 / 观测）
};

}  // namespace mmo::game::combat
