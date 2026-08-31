/// TASK-016 · 属性三层模型实现（§8 / §15.1）。
///
/// 进程级配置（钳制区间 / 派生公式）以全局表承载：启动期写入、运行期只读，
/// 因此**不占角色内存**（§22 单角色 < 512B 预算）。

#include "mmo/game/role/attribute.h"

#include <algorithm>

namespace mmo::game::role {
namespace {

using LimitTable = std::array<AttrLimit, kAttrCount>;

/// 默认钳制区间（单一事实来源，集中在此便于评审与调整）。
LimitTable DefaultLimits() noexcept {
    LimitTable t{};
    auto set = [&t](AttrType a, std::int64_t lo, std::int64_t hi) {
        t[static_cast<std::size_t>(a)] = AttrLimit{lo, hi};
    };
    // 主属性：0 .. 9999
    set(AttrType::Strength, 0, 9999);
    set(AttrType::Agility, 0, 9999);
    set(AttrType::Intellect, 0, 9999);
    set(AttrType::Stamina, 0, 9999);
    // 派生属性
    set(AttrType::MaxHp, 1, 10'000'000);
    set(AttrType::MaxMp, 0, 10'000'000);
    set(AttrType::Attack, 0, 1'000'000);
    set(AttrType::Defense, 0, 1'000'000);
    set(AttrType::CritRate, 0, 10'000);       // 万分比，上限 100%
    set(AttrType::CritDamage, 0, 100'000);    // 万分比
    set(AttrType::MoveSpeed, 0, 100'000);     // 毫米/秒
    return t;
}

LimitTable& MutableLimits() noexcept {
    static LimitTable g = DefaultLimits();
    return g;
}

AttrFormula& MutableFormula() noexcept {
    static AttrFormula g{};
    return g;
}

inline std::int64_t Clamp(std::int64_t v, std::int64_t lo, std::int64_t hi) noexcept {
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

}  // namespace

AttrLimit DefaultAttrLimit(AttrType t) noexcept {
    return DefaultLimits()[static_cast<std::size_t>(t)];
}

AttrLimit AttrLimitFor(AttrType t) noexcept {
    return MutableLimits()[static_cast<std::size_t>(t)];
}

void SetAttrLimit(AttrType t, AttrLimit limit) noexcept {
    MutableLimits()[static_cast<std::size_t>(t)] = limit;
}

void ResetAttrLimits() noexcept { MutableLimits() = DefaultLimits(); }

const AttrFormula& AttrFormulaFor() noexcept { return MutableFormula(); }
void SetAttrFormula(AttrFormula f) noexcept { MutableFormula() = f; }
void ResetAttrFormula() noexcept { MutableFormula() = AttrFormula{}; }

void AttributeSet::Recompute() noexcept {
    const AttrFormula& f = AttrFormulaFor();

    // 1. 主属性：三层求和 + 钳制。
    for (std::size_t i = 0; i < kPrimaryAttrCount; ++i) {
        const auto type = static_cast<AttrType>(i);
        const AttrLimit lim = AttrLimitFor(type);
        const std::int64_t raw = base[i] + from_equipment[i] + from_buff[i];
        total_[i] = Clamp(raw, lim.min, lim.max);
    }

    // 2. 派生属性：由主属性 Final 按配置化公式计算（忽略直接写入这三层的派生值，
    //    派生属性的 base/equipment/buff 仍可作为"固定加成"叠加，见下）。
    const std::int64_t str = total_[static_cast<std::size_t>(AttrType::Strength)];
    const std::int64_t agi = total_[static_cast<std::size_t>(AttrType::Agility)];
    const std::int64_t inl = total_[static_cast<std::size_t>(AttrType::Intellect)];
    const std::int64_t sta = total_[static_cast<std::size_t>(AttrType::Stamina)];

    auto derived = [this](AttrType t, std::int64_t value) {
        const std::size_t i = static_cast<std::size_t>(t);
        const AttrLimit lim = AttrLimitFor(t);
        // 公式结果 + 三层固定加成（装备/Buff 给的"最大生命 +500"这类直加值）
        const std::int64_t raw = value + base[i] + from_equipment[i] + from_buff[i];
        total_[i] = Clamp(raw, lim.min, lim.max);
    };

    derived(AttrType::MaxHp, f.hp_per_stamina * sta + f.hp_base);
    derived(AttrType::MaxMp, f.mp_per_intellect * inl + f.mp_base);
    derived(AttrType::Attack, f.atk_per_strength * str + f.atk_base);
    derived(AttrType::Defense, f.def_per_agility * agi + f.def_base);
    derived(AttrType::CritRate, f.crit_per_agility * agi + f.crit_base);
    derived(AttrType::CritDamage, f.critdmg_per_intellect * inl + f.critdmg_base);
    derived(AttrType::MoveSpeed, f.movespeed_per_agility * agi + f.movespeed_base);
}

}  // namespace mmo::game::role
