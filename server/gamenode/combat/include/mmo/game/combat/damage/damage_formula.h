#pragma once

/// TASK-022 · 伤害公式（§8 / §15.2 / §20.1）。
///
/// 三条硬约束：
///   ① **全部参数配置化**：系数只从 `config/gameplay/combat/formula.json` 读取，
///      本头文件与 .cpp 里不得出现任何魔法数字（§21 / §20.1）。
///   ② **纯函数、无随机**：暴击/闪避的随机量由调用方以 `DamageRolls` 传入（§15.2），
///      本文件内**不持有、不推进**任何 PRNG 状态 —— ComputeDamage 因此可在任意线程调用（§9）。
///   ③ **整数运算、无溢出**：所有中间量先钳制到 `max_raw_damage` 再做乘除，
///      保证 `raw * scale` 与 `raw * K` 都远小于 2^63（§19「禁止产生整数溢出」）。
///
/// 结算顺序（§20.2，固定，禁止调整）：
///   暴击判定 → 闪避判定 → 抗性减免 → 护盾吸收 → 扣 HP → 致死判定 → 发布事件
///
/// 依赖方向（§27.3）：本头只依赖 core 与本模块 damage.h，
/// **不依赖 role** —— 属性到公式入参的映射由 DamageSystem 负责，公式层保持纯算术。

#include <cstdint>
#include <string_view>

#include "mmo/core/error/result.h"
#include "mmo/game/combat/damage/damage.h"

namespace mmo::game::combat {

/// 公式参数（= formula.json 的 1:1 映射）。
///
/// **禁止默认值静默启动**：`DamageSystem` 必须由调用方显式传入已加载成功的公式
/// （构造函数无默认实参），缺文件/缺字段一律 `LoadFromFile` 失败（TASK-016 同款闸门）。
struct DamageFormula {
    /// 万分比分母。与 TASK-016 `AttrFormula` 的 CritRate / CritDamage 单位一致（10000 = 100.00%）。
    std::int64_t rate_scale{10000};
    /// 属性里 CritDamage 为 0（未配置）时使用的暴击伤害（万分比，15000 = 1.5x）。
    /// 这是**配置里显式声明的兜底值**，不是代码硬编码。
    std::int64_t crit_damage_default{15000};
    /// 暴击率上限（万分比），防止属性异常导致必爆。
    std::int64_t crit_rate_max{9500};

    // ---- 闪避（§8 `DodgeRate`）----
    /// TASK-016 的 `AttrType` 没有 DodgeRate，故本版按敏捷派生：
    /// `dodge_rate = dodge_rate_base + dodge_per_agility * Agility`，再钳制到 `dodge_rate_max`。
    /// 三项全在配置里；TASK-016 若补上 DodgeRate 属性，改配置即可整体切换，无需改代码。
    std::int64_t dodge_rate_base{200};
    std::int64_t dodge_per_agility{3};
    std::int64_t dodge_rate_max{5000};

    // ---- 抗性减免：`mitigated = raw * K / (K + Defense)`（§8）----
    std::int64_t mitigation_k{400};
    /// 减免上限（万分比）：7500 = 最多减掉 75%，保证高护甲也不能免疫。
    std::int64_t mitigation_max{7500};

    /// 保底伤害：raw > 0 但减免后为 0 时提升到该值（避免"打不动"的退化手感）。
    std::int64_t min_damage{1};
    /// raw 上限（防 int64 溢出）：所有乘除之前先把 raw 钳到这里。
    std::int64_t max_raw_damage{1000000};

    // ---- 真实伤害（TrueDamage）行为开关 ----
    bool true_damage_ignores_mitigation{true};
    bool true_damage_ignores_dodge{true};
    bool true_damage_can_crit{false};

    /// 护盾先于 HP 扣除（§15.5）。
    bool shield_absorbs_before_hp{true};
    /// 致死阈值：HP ≤ 该值判死（0 = 归零即死）。
    std::int64_t lethal_hp_threshold{0};

    /// 满血治疗是否计入统计与事件（见 `HealRequest::overheal_allowed` 文档）。
    bool heal_overheal_counts_to_stats{true};

    // ---- 采样日志（§15.8）----
    std::int64_t log_samples_per_second{100};
    std::int64_t log_ring_capacity{1024};
    /// Tick 频率（把"每秒 N 条"换算成"每 Tick N/HZ 条"），与 TASK-013 的 20Hz 一致。
    std::int64_t tick_rate_hz{20};

    /// 从 JSON 文件加载，返回**值快照**（与进程级 ConfigManager 解耦，DamageSystem 持有自己的副本）。
    ///
    /// 走 `core::ConfigManager`（TASK-003），与 TASK-016 `exp_curve` 同套路：
    /// 文件 IO 留在 core，**本模块 src/ 不出现 std::ifstream**（§24 静态红线扫描）。
    /// 键路径为点号形式 `damage.<field>`，避免与其它配置文件键冲突。
    /// 缺文件 / 缺字段 / 类型不符一律 Fail（**禁止默认值静默生成**，TASK-016 教训）。
    /// 幂等：键已存在时跳过 LoadFile，重复调用安全。
    static core::Result<DamageFormula> LoadFromFile(std::string_view path) noexcept;

    /// 自检：字段取值是否自洽（rate_scale > 0、mitigation_k > 0、区间合法等）。
    core::Result<void> Validate() const noexcept;
};

/// 随机量输入（§15.2「随机由调用方传入」）。
///
/// 两个 roll 均来自 per-Scene 确定性 PRNG 的 `NextScaled(rate_scale)`，
/// 语义是 [0, rate_scale) 的均匀分布。
struct DamageRolls {
    std::uint32_t crit{0};
    std::uint32_t dodge{0};
};

// ============================================================================
// 纯函数公式层（noexcept、无状态、无随机、无分配 —— 全部可在编译期外直接单测）
// ============================================================================

/// 把 raw 钳到 [0, max_raw_damage]。所有乘除的前置步骤（§19 防溢出）。
inline std::int64_t ClampRaw(std::int64_t raw, const DamageFormula& f) noexcept {
    if (raw < 0) return 0;
    if (raw > f.max_raw_damage) return f.max_raw_damage;
    return raw;
}

/// 闪避率（万分比）＝ base + per_agility * Agility，钳到 dodge_rate_max。
inline std::int64_t DodgeRateOf(std::int64_t agility, const DamageFormula& f) noexcept {
    std::int64_t rate = f.dodge_rate_base + f.dodge_per_agility * agility;
    if (rate < 0) rate = 0;
    if (rate > f.dodge_rate_max) rate = f.dodge_rate_max;
    return rate;
}

/// 【第 1 步】暴击判定：`roll < crit_rate` ⇒ raw × CritDamage / rate_scale。
/// crit_rate / crit_damage 均为万分比整数，返回值是暴击后的 raw。
inline std::int64_t ApplyCrit(std::int64_t raw, std::int64_t crit_rate, std::int64_t crit_damage,
                              std::uint32_t roll, const DamageFormula& f, bool* is_crit) noexcept {
    *is_crit = false;
    if (crit_rate <= 0 || raw <= 0) return raw;
    if (crit_rate > f.crit_rate_max) crit_rate = f.crit_rate_max;
    if (roll >= static_cast<std::uint32_t>(crit_rate)) return raw;
    *is_crit = true;
    // raw ≤ max_raw_damage(1e6) 且 crit_damage 为万分比(≤1e5) ⇒ 乘积 ≤ 1e11，int64 安全。
    return raw * crit_damage / f.rate_scale;
}

/// 【第 2 步】闪避判定：`roll < dodge_rate`。
inline bool IsDodged(std::int64_t dodge_rate, std::uint32_t roll) noexcept {
    return dodge_rate > 0 && roll < static_cast<std::uint32_t>(dodge_rate);
}

/// 【第 3 步】抗性减免：`mitigated = raw * K / (K + Defense)`，并受减免上限保护。
///
/// 减免上限的含义：`mitigation_max = 7500` ⇒ 最多减掉 75%，即 mitigated ≥ raw 的 25%。
/// 实现为「先算 floor 除法，再用下限校正」，避免高护甲把伤害压成 0 而绕过保底伤害。
inline std::int64_t Mitigate(std::int64_t raw, std::int64_t defense,
                             const DamageFormula& f) noexcept {
    if (raw <= 0) return 0;
    const std::int64_t k = f.mitigation_k;
    const std::int64_t def = defense > 0 ? defense : 0;
    // raw ≤ 1e6，k ≤ 1e5 ⇒ raw * k ≤ 1e11，int64 安全。
    std::int64_t mitigated = raw * k / (k + def);
    // 减免下限：mitigated ≥ raw * (rate_scale - mitigation_max) / rate_scale
    const std::int64_t floor_by_cap = raw * (f.rate_scale - f.mitigation_max) / f.rate_scale;
    if (mitigated < floor_by_cap) mitigated = floor_by_cap;
    // 保底伤害（raw > 0 时不允许被减免到 0）
    if (mitigated < f.min_damage) mitigated = f.min_damage;
    if (mitigated > raw) mitigated = raw;
    return mitigated;
}

/// 【第 4 步】护盾吸收：`absorbed = min(mitigated, shield)`，`final = mitigated - absorbed`。
/// `is_blocked` 置位条件：mitigated > 0 且被护盾**完全**吸收（§15.5）。
inline std::int64_t Absorb(std::int64_t mitigated, std::int64_t shield, const DamageFormula& f,
                           std::int64_t* absorbed, bool* is_blocked) noexcept {
    *absorbed = 0;
    *is_blocked = false;
    if (!f.shield_absorbs_before_hp || shield <= 0) return mitigated;
    *absorbed = mitigated < shield ? mitigated : shield;
    if (mitigated > 0 && *absorbed >= mitigated) *is_blocked = true;
    return mitigated - *absorbed;
}

/// 治疗：raw → 钳制到 [0, MaxHp - hp]，返回实际恢复量与溢出量。
inline void SettleHeal(std::int64_t raw, std::int64_t hp, std::int64_t max_hp,
                       std::int64_t* effective, std::int64_t* overheal) noexcept {
    std::int64_t room = max_hp - hp;
    if (room < 0) room = 0;
    *effective = raw < room ? raw : room;
    *overheal = raw - *effective;
    if (*overheal < 0) *overheal = 0;
}

}  // namespace mmo::game::combat
