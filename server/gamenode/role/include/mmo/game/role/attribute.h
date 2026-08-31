#pragma once

/// TASK-016 · 属性三层模型（§7 / §8）。
///
/// 设计铁律（§21）：**禁止任何系统直接改 Final 值**。
/// 三层来源 base / from_equipment / from_buff 可被各自归属系统写入：
///   · base         —— 等级与种族决定，由 RoleSystem 在升级时变更（本任务）
///   · from_equipment —— TASK-017 装备系统写入（本任务只预留空位）
///   · from_buff    —— TASK-023 Buff 系统写入（本任务只预留空位）
/// 任何一层变更后必须调用 Recompute()，由它生成派生层 total_（唯一 Final，私有）。
///
/// 派生语义：主属性（Strength/Agility/Intellect/Stamina）的 Final = 三层求和并钳制；
/// 派生属性（MaxHp/MaxMp/Attack/Defense/CritRate/CritDamage/MoveSpeed）由主属性 Final
/// 经配置化公式算出，同样是 Recompute() 独占写入。

#include <array>
#include <cstddef>
#include <cstdint>

namespace mmo::game::role {

/// 属性类型（§7 冻结顺序，索引即存储下标，禁止重排）。
enum class AttrType : std::uint8_t {
    Strength = 0,
    Agility,
    Intellect,
    Stamina,
    MaxHp,
    MaxMp,
    Attack,
    Defense,
    CritRate,
    CritDamage,
    MoveSpeed,
};

inline constexpr std::size_t kAttrCount = 11;

/// 主属性（前 4 个）与派生属性（其余）的分界。
inline constexpr std::size_t kPrimaryAttrCount = 4;

inline bool IsPrimaryAttr(AttrType t) noexcept {
    return static_cast<std::size_t>(t) < kPrimaryAttrCount;
}

inline const char* AttrName(AttrType t) noexcept {
    switch (t) {
        case AttrType::Strength:   return "Strength";
        case AttrType::Agility:    return "Agility";
        case AttrType::Intellect:  return "Intellect";
        case AttrType::Stamina:    return "Stamina";
        case AttrType::MaxHp:      return "MaxHp";
        case AttrType::MaxMp:      return "MaxMp";
        case AttrType::Attack:     return "Attack";
        case AttrType::Defense:    return "Defense";
        case AttrType::CritRate:   return "CritRate";
        case AttrType::CritDamage: return "CritDamage";
        case AttrType::MoveSpeed:  return "MoveSpeed";
    }
    return "Unknown";
}

/// 属性钳制区间（§8「带钳制」）。进程级配置，启动期写入、运行期只读，故不占角色内存。
struct AttrLimit {
    std::int64_t min{0};
    std::int64_t max{0};
};

/// 默认钳制区间（单一事实来源；运行期可用 SetAttrLimit 覆盖，见 attribute.cpp）。
AttrLimit DefaultAttrLimit(AttrType t) noexcept;
/// 当前生效的钳制区间。
AttrLimit AttrLimitFor(AttrType t) noexcept;
/// 覆盖某属性的钳制区间（启动期配置用；运行期调用不保证线程安全，§9 单 Owner）。
void SetAttrLimit(AttrType t, AttrLimit limit) noexcept;
void ResetAttrLimits() noexcept;

/// 派生属性公式系数（配置化，禁止散落在 Recompute 里）。
struct AttrFormula {
    // MaxHp  = hp_per_stamina * Stamina + hp_base
    std::int64_t hp_per_stamina{10};
    std::int64_t hp_base{50};
    // MaxMp  = mp_per_intellect * Intellect + mp_base
    std::int64_t mp_per_intellect{8};
    std::int64_t mp_base{30};
    // Attack = atk_per_strength * Strength + atk_base
    std::int64_t atk_per_strength{2};
    std::int64_t atk_base{5};
    // Defense = def_per_agility * Agility + def_base
    std::int64_t def_per_agility{1};
    std::int64_t def_base{2};
    // CritRate（万分比）= crit_per_agility * Agility + crit_base
    std::int64_t crit_per_agility{5};
    std::int64_t crit_base{300};
    // CritDamage（万分比）= critdmg_per_intellect * Intellect + critdmg_base
    std::int64_t critdmg_per_intellect{3};
    std::int64_t critdmg_base{15000};
    // MoveSpeed（毫米/秒）= movespeed_per_agility * Agility + movespeed_base
    std::int64_t movespeed_per_agility{4};
    std::int64_t movespeed_base{6000};
};

const AttrFormula& AttrFormulaFor() noexcept;
void SetAttrFormula(AttrFormula f) noexcept;
void ResetAttrFormula() noexcept;

/// 三层属性集（§7）。sizeof = 4 × 11 × 8 = 352B。
struct AttributeSet {
    std::array<std::int64_t, kAttrCount> base{};            // 等级/种族（RoleSystem 写）
    std::array<std::int64_t, kAttrCount> from_equipment{};  // TASK-017 装备系统写
    std::array<std::int64_t, kAttrCount> from_buff{};       // TASK-023 Buff 系统写

    /// 读 Final（派生层，O(1)）。
    std::int64_t Total(AttrType t) const noexcept {
        return total_[static_cast<std::size_t>(t)];
    }

    /// 由三层来源重算派生层（O(k)，k=11）。**任一层变更后必须调用**（§8）。
    void Recompute() noexcept;

private:
    /// 派生层（Final）：唯一写入者是 Recompute()，外部不可直接改（§21）。
    std::array<std::int64_t, kAttrCount> total_{};
};
static_assert(sizeof(AttributeSet) == 352, "AttributeSet 布局固定为 4 × 11 × int64");

}  // namespace mmo::game::role
