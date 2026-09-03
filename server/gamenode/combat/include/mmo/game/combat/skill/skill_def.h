#pragma once

/// TASK-021 · Skill 定义（§7 / §15.1 / §15.2）。全部配置化（§21 Forbidden 硬编码）。
///
/// 注意：任务书 §7 写作 `entity::EntityId` / `scene::SceneContext`，实际类型定义在 TASK-011
/// 实体模块的 `mmo::game` 命名空间；本文件用真实类型别名对齐（禁止重复定义第二套）。

#include <cstdint>
#include <string>
#include <vector>

#include "mmo/core/time/clock.h"

namespace mmo::game::combat {

using SkillId = std::uint32_t;

/// 目标类型（§7）：四类 = Self / SingleTarget / AOE(circle|cone) / Projectile。
enum class TargetType : std::uint8_t {
    Self,
    SingleTarget,
    AoeCircle,
    AoeCone,
    Projectile,
};

/// TryCast 结果（§7 / §16）。所有分支须可单测到达（§20 #2）。
enum class CastResult : std::uint8_t {
    Ok,
    OnCooldown,
    OutOfRange,
    NoTarget,
    InsufficientResource,
    Interrupted,
    InvalidTarget,
    Silenced,
};

/// 效果类型（§8）。
enum class EffectType : std::uint8_t {
    Damage,
    Heal,
    ApplyBuff,
    SpawnProjectile,
};

/// 单个效果（§8）。
struct EffectDef {
    EffectType type{EffectType::Damage};
    std::uint32_t school{0};        // 伤害系（fire/ice...）第一版仅标识
    float base{0.0f};
    float coeff{0.0f};
    bool can_crit{false};
    std::uint32_t buff_id{0};       // ApplyBuff 引用
    std::uint32_t duration_ms{0};   // ApplyBuff / SpawnProjectile 上限
    std::uint16_t stacks{1};
    float speed{10.0f};             // SpawnProjectile：飞行速度 m/s
    float radius{1.5f};             // SpawnProjectile 命中半径
    float max_distance{30.0f};      // SpawnProjectile 最大飞行距离
};

/// 技能定义（§7）。数值全部来自 skills.json，代码零硬编码（§21）。
struct SkillDef {
    SkillId id{0};
    std::string name;
    TargetType target_type{TargetType::SingleTarget};
    float cast_time{0.0f};          // 0 = 瞬发
    float cooldown{1.5f};           // 秒
    float range{5.0f};              // 米
    float radius{0.0f};             // AOE 半径（circle/cone）
    float cone_deg{90.0f};          // AOE cone 张角（度）
    std::int64_t mana_cost{0};
    std::int64_t hp_cost{0};
    std::vector<EffectDef> effects;
    bool interruptible{true};       // 预留（打断策略由 Combat 决定）
    std::uint32_t required_level{1};
    std::uint32_t index_{0};        // 紧凑内部索引（CooldownTracker O(1) 用，§15.3）
};

}  // namespace mmo::game::combat
