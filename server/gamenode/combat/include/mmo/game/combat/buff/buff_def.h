#pragma once

/// TASK-023 · Buff / Debuff 定义（§7 / §8 / §15）。
///
/// 命名空间隔离（重要）：TASK-021 已在 `mmo::game::combat` 冻结了 `combat::BuffDef`
///（`skill/buff_def.h`，仅做 id 校验的占位结构）。本任务的完整 Buff 定义放在独立的
/// `mmo::game::combat::buff` 命名空间，**不重用、不扩展** TASK-021 的占位结构，
/// 以避免破坏其已冻结的契约（§27.1）。
///
/// 属性模型（§8 / TASK-016 §21）：Buff 只写 `AttributeSet::from_buff` 一层，绝不直接改 Final。
/// 加法（attr_modifiers）与乘法（attr_multipliers「先加后乘」）在 Apply/Remove 时合并成
/// `from_buff` 的净增量，再调 `RoleSystem::RecomputeAttributes` 重算派生层。
///
/// 红线（§27.3）：本公开头只依赖 role / core 的公开头，禁止 include 任何 `src/`。

#include <cstdint>
#include <string>

#include "mmo/core/error/result.h"
#include "mmo/game/role/attribute.h"

namespace mmo::game::combat::buff {

/// Buff 类别（kinds）。
enum class BuffKind : std::uint8_t {
    Buff = 0,     // 增益
    Debuff = 1,   // 减益
    Control = 2,  // 控制（眩晕/定身/沉默/无敌）
    Shield = 3,  // 护盾
};

/// 堆叠规则（§8）。
enum class StackRule : std::uint8_t {
    None = 0,         // 不叠加：重复施加只刷新持续时间（最多 1 层）
    Refresh = 1,      // 刷新：重复施加刷新持续时间，层数 +1 直到 max_stacks
    Independent = 2,  // 独立：每次施加生成独立实例（上限 max_stacks，超出顶掉最旧）
};

/// 控制标记位（供 TASK-024 CombatSystem 查询角色是否被控）。
enum class ControlFlag : std::uint8_t {
    Stunned = 1,        // 眩晕：不可行动/施法
    Rooted = 1 << 1,    // 定身：不可移动
    Silenced = 1 << 2,  // 沉默：不可施法
    Invulnerable = 1 << 3,  // 无敌：不受伤害
};

/// 周期性效果（DOT / HOT / 护盾脉冲）。
struct TickEffect {
    std::int64_t amount{0};   // 每 tick 的伤害/治疗**量（正数 magnitude）**；0 = 无周期效果
    std::uint8_t school{0};   // DamageSchool 编码（0 Physical / 1 Magical / 2 TrueDamage）
    bool can_crit{false};
    bool is_heal{false};      // true = HOT（治疗）；false = DOT（伤害）
};

/// 单个 Buff 的静态定义（注册表持有，运行期只读，指针稳定）。
struct BuffDef {
    std::uint32_t id{0};
    std::string name;
    BuffKind kind{BuffKind::Buff};
    StackRule stack_rule{StackRule::Refresh};
    std::uint16_t max_stacks{1};
    std::uint32_t duration_ms{0};       // 0 = 永久
    std::uint32_t tick_interval_ms{0};  // 0 = 无周期效果
    std::int64_t attr_modifiers[role::kAttrCount]{};  // 加法贡献（写 from_buff），单位与属性一致
    double attr_multipliers[role::kAttrCount]{};       // 乘法贡献（1.0 = 无；0.1 = +10%），引用「无 Buff 基准值」
    std::int64_t shield_value{0};       // 施加时给目标的护盾量（仅 Shield 类使用）
    TickEffect tick_effect{};
    bool dispellable{true};
    std::uint8_t control_mask{0};       // ControlFlag 位掩码（仅 Control 类非零）
};

/// 移除原因（事件 / 日志用）。
enum class RemoveReason : std::uint8_t {
    Expired = 0,
    Dispelled = 1,
    Replaced = 2,   // Independent 规则顶掉最旧实例
    Death = 3,
    Manual = 4,
};

}  // namespace mmo::game::combat::buff
