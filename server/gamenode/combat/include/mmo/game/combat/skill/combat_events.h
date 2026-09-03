#pragma once

/// TASK-021 · Combat 事件（§8 / §15.5）。
///
/// 事件须为 POD、≤32B、nothrow move，供 EventBus 内联发布（TASK-007）。
/// DamageEvent / HealEvent 由 TASK-022 伤害系统消费；本任务已同步经 RoleSystem.ModifyHp
/// 实际扣血，事件作为下游/日志用途。

#include <cstdint>

#include "mmo/core/log/trace_id.h"
#include "mmo/game/combat/skill/skill_def.h"

namespace mmo::game::combat {

/// 施法开始（读条开始即发布，供打断系统订阅）。28B
struct SkillCast {
    EntityId caster{0};
    SkillId  skill{0};
    EntityId target{0};
    core::TraceID trace{0};
};
static_assert(sizeof(SkillCast) <= 32, "SkillCast must be <=32B for EventBus");

enum class InterruptReason : std::uint8_t { Hit, Stun, Death, Manual };

/// 读条被打断（资源不退，§15.7 / §21 Forbidden）。21B
struct SkillInterrupted {
    EntityId caster{0};
    SkillId  skill{0};
    InterruptReason reason{InterruptReason::Manual};
    core::TraceID trace{0};
};
static_assert(sizeof(SkillInterrupted) <= 32);

/// 伤害事件：target 实际掉血由 RoleSystem.ModifyHp 同步执行，本事件供 TASK-022/日志。32B
struct DamageEvent {
    EntityId caster{0};
    EntityId target{0};
    std::int32_t amount{0};       // 实际生效伤害（正数，int32 足够：单跳伤害量级远小于 2^31）
    std::uint32_t packed{0};      // bit0: can_crit；bit8..15: school
    core::TraceID trace{0};
};
static_assert(sizeof(DamageEvent) <= 32);

/// 治疗事件。24B
struct HealEvent {
    EntityId caster{0};
    EntityId target{0};
    std::int64_t amount{0};
    core::TraceID trace{0};
};
static_assert(sizeof(HealEvent) <= 32);

/// Buff 施加意图（实际施加在 TASK-023）。26B
struct BuffApplied {
    EntityId caster{0};
    EntityId target{0};
    std::uint32_t buff_id{0};
    std::uint32_t duration_ms{0};
    std::uint16_t stacks{1};
};
static_assert(sizeof(BuffApplied) <= 32);

/// 把 EffectDef 的 school / can_crit 压进 DamageEvent.packed。
inline std::uint32_t PackFx(std::uint32_t school, bool can_crit) noexcept {
    return (school & 0xFFu) | (can_crit ? 0x100u : 0u);
}

}  // namespace mmo::game::combat
