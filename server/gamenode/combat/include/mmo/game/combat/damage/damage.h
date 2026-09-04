#pragma once

/// TASK-022 · Damage / Heal 数据结构与事件（§7 / §8 / §15.1）。
///
/// 命名空间说明：任务书 §7 写作 `entity::EntityId` / `scene::SceneContext`，实际类型定义在
/// TASK-011 实体模块的 `mmo::game` 命名空间；本文件用真实类型对齐（禁止重复定义第二套）。
///
/// ── 与 TASK-021 的命名冲突处理（重要）────────────────────────────────────────
/// 任务书 §7 声明了 `struct DamageEvent { source, target, school, DamageResult result,
/// tick_number, trace }`。但：
///   ① `mmo::game::combat::DamageEvent` 已在 TASK-021 定义并冻结（§27.1 契约冻结），
///      它是 EventBus 的**总线事件**，必须 ≤32B（EventSlot::kInlinePayload = 32，
///      见 core/bus/event_slot.h）——超过即退化为堆分配，直接违反 §21「禁止伤害结算产生
///      堆分配（alloc_per_damage = 0）」。
///   ② 内含完整 DamageResult 的事件约 72B，无法内联。
/// 因此本任务采取如下分解，语义与 §7 完全一致，只是载体不同：
///   · `DamageEvent`（TASK-021，32B）      —— 总线事件，热路径零分配，保持不变；
///   · `DamageResult`（本文件，§7 完整字段）—— ComputeDamage / ApplyDamage 的**返回值**；
///   · `DamageRecord`（本文件，§7 DamageEvent 的字段集）—— **完整结算记录**，用于采样日志
///     与离线对账，**不进 EventBus**，故不受 32B 约束、不产生热路径分配。
/// 该分解在 docs/INTERFACE.md §「与 TASK-021 的契约协调」中同步记录。

#include <cstdint>

#include "mmo/core/log/trace_id.h"
#include "mmo/game/entity/entity_id.h"

namespace mmo::game::combat {

/// 伤害系（§7）。第一版三系；新增系别须走注册表，禁止在 switch 里穷举（§27.4）。
enum class DamageSchool : std::uint8_t {
    Physical = 0,
    Magical = 1,
    TrueDamage = 2,
};

/// 伤害请求（§7）。base_amount + coefficient * AttackPower 构成 raw。
/// coefficient 为 float：配置表里技能系数天然是小数；NaN/Inf 由 ApplyDamage 前置拒绝（§19）。
struct DamageRequest {
    EntityId source{0};
    EntityId target{0};
    DamageSchool school{DamageSchool::Physical};
    std::int64_t base_amount{0};
    float coefficient{0.0f};
    bool can_crit{true};
    bool can_be_dodged{true};
    core::TraceID trace{0};
    core::RequestID request_id{0};
};

/// 伤害结算结果（§7）。字段含义见 §8 公式与 docs/INTERFACE.md。
///
/// `is_blocked` 第一版语义 = 伤害被护盾**完全**吸收（mitigated > 0 且 absorbed >= mitigated）；
/// 真正的「格挡」机制留待 TASK-023，届时不得复用本字段改语义（§27.1）。
struct DamageResult {
    std::int64_t raw{0};            // base + coeff * Attack（暴击前）
    std::int64_t mitigated{0};      // 抗性减免后、护盾吸收前
    std::int64_t absorbed{0};       // 被护盾吃掉的部分
    std::int64_t final_amount{0};   // 实际扣血量（= mitigated - absorbed，闪避为 0）
    bool is_crit{false};
    bool is_dodged{false};
    bool is_blocked{false};
    std::int64_t remaining_hp{0};   // 扣血后目标剩余 HP（已钳制 ≥ 0）
    bool lethal{false};             // 本次结算导致目标死亡
};

/// 治疗请求（§7）。
struct HealRequest {
    EntityId source{0};
    EntityId target{0};
    std::int64_t base_amount{0};
    float coefficient{0.0f};
    bool can_crit{true};
    /// false（默认）：目标满血时视为无效治疗——仍返回结果，但**不发布 HealEvent、
    /// 不计入 total_heal**（防止刷治疗量统计，§6 观测性）。
    /// true：满血也发布事件，并把溢出量计入 `total_overheal`。
    bool overheal_allowed{false};
    core::TraceID trace{0};
};

/// 治疗结算结果（§7）。effective 恒为**实际恢复的 HP**；overheal = raw - effective ≥ 0。
struct HealResult {
    std::int64_t raw{0};
    std::int64_t effective{0};
    std::int64_t overheal{0};
    std::int64_t remaining_hp{0};
    bool is_crit{false};
};

/// 完整伤害结算记录（对应任务书 §7 的 DamageEvent 字段集）。
///
/// **不是** EventBus 事件：体积 72B，若进总线会退化为堆分配。它只在
/// ① `ApplyDamage` 的可选 out 参数、② 采样日志（§15.8）中流转。
struct DamageRecord {
    EntityId source{0};
    EntityId target{0};
    DamageSchool school{DamageSchool::Physical};
    DamageResult result{};
    std::uint64_t tick_number{0};
    core::TraceID trace{0};
};

/// 实体死亡（§15.6）。战斗侧事件，与 Role 的 `CharacterDied`（角色 ID 维度）**语义不同**：
/// 本事件以实体 ID 为准，驱动战斗/AI 侧反应；角色死亡与落盘由 Role 侧事件负责。
/// 24B —— 必须保持 ≤32B 才能走 EventBus 内联路径。
struct EntityDied {
    EntityId entity{0};
    EntityId killer{0};
    core::TraceID trace{0};
};
static_assert(sizeof(EntityDied) <= 32, "EntityDied must be <=32B for EventBus inline payload");

}  // namespace mmo::game::combat
