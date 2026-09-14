/// TASK-023 · BuffSystem 实现（§7 / §8 / §15 / §21）。
///
/// 热路径（§10）：Tick 禁止后端存储 / 网络 / 文件 IO / 堆分配 / 线程创建；事件 ≤32B 内联；
/// 周期结算（DOT/HOT）经 DamageSystem 走统一结算（§15），本文件不重复结算逻辑。
///
/// 红线（§24 静态扫描）：本文件热路径不得引入任何后端存储、网络、文件读取或线程创建依赖。

#include "mmo/game/combat/buff/buff_system.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/game/combat/buff/buff_events.h"
#include "mmo/game/combat/damage/damage.h"
#include "mmo/game/combat/skill/combat_events.h"  // 复用冻结的 combat::BuffApplied（§21）
#include "mmo/game/role/attribute.h"
#include "mmo/game/role/character.h"

namespace mmo::game::combat::buff {

namespace {
using core::Error;
using core::ErrorCode;

template <typename T>
core::Result<T> Fail(ErrorCode c, const char* msg) {
    return core::Result<T>::Fail(Error(c, msg, core::domain::kCore));
}

inline std::chrono::milliseconds Ms(std::uint64_t ms) noexcept {
    return std::chrono::milliseconds(ms);
}

/// 永久（无到期 / 无周期）哨兵。
constexpr core::SteadyTime kForever = core::SteadyTime::max();

}  // namespace

// ===========================================================================
// BuffRegistry
// ===========================================================================

core::Result<void> BuffRegistry::Add(BuffDef def) {
    if (defs_.find(def.id) != defs_.end()) {
        return Fail<void>(ErrorCode::INVALID_ARGUMENT, "buff id already registered");
    }
    defs_.emplace(def.id, std::move(def));
    return core::Result<void>::Ok();
}

const BuffDef* BuffRegistry::Find(std::uint32_t id) const noexcept {
    auto it = defs_.find(id);
    return it == defs_.end() ? nullptr : &it->second;
}

// ===========================================================================
// BuffSystem
// ===========================================================================

BuffSystem::BuffSystem(const BuffRegistry& defs, role::RoleSystem& roles,
                       combat::DamageSystem* dmg, core::EventBus* bus)
    : defs_(defs), roles_(roles), dmg_(dmg), bus_(bus) {}

void BuffSystem::BindAvatar(EntityId entity, role::CharacterId character) noexcept {
    avatar_of_[entity] = character;
    char_to_entity_[character] = entity;
}

// ---- IShieldSource ----
std::int64_t BuffSystem::ShieldOf(EntityId target) const noexcept {
    auto ait = avatar_of_.find(target);
    if (ait == avatar_of_.end()) return 0;
    auto bit = by_char_.find(ait->second);
    if (bit == by_char_.end()) return 0;
    std::int64_t sum = 0;
    for (const auto& inst : bit->second) {
        if (inst.shield_remaining > 0) sum += inst.shield_remaining;
    }
    return sum;
}

void BuffSystem::ConsumeShield(EntityId target, std::int64_t amount) noexcept {
    if (amount <= 0) return;
    auto ait = avatar_of_.find(target);
    if (ait == avatar_of_.end()) return;
    auto bit = by_char_.find(ait->second);
    if (bit == by_char_.end()) return;
    std::int64_t remaining = amount;
    for (auto& inst : bit->second) {
        if (remaining <= 0) break;
        if (inst.shield_remaining <= 0) continue;
        if (inst.shield_remaining >= remaining) {
            inst.shield_remaining -= remaining;
            remaining = 0;
        } else {
            remaining -= inst.shield_remaining;
            inst.shield_remaining = 0;
        }
    }
}

// ---- 属性重算（先加后乘）----
void BuffSystem::RecomputeFromBuff(role::Character* c) const {
    if (c == nullptr) return;
    // 1. 无 Buff 基准值 = base + equipment（主属性与派生属性通用表达）
    std::array<std::int64_t, role::kAttrCount> ref{};
    for (std::size_t i = 0; i < role::kAttrCount; ++i) {
        ref[i] = c->attrs.base[i] + c->attrs.from_equipment[i];
    }
    // 派生属性的无 Buff 基准 = 公式(主属性) + base + equipment
    const role::AttrFormula& f = role::AttrFormulaFor();
    const std::int64_t str = ref[static_cast<std::size_t>(role::AttrType::Strength)];
    const std::int64_t agi = ref[static_cast<std::size_t>(role::AttrType::Agility)];
    const std::int64_t inl = ref[static_cast<std::size_t>(role::AttrType::Intellect)];
    const std::int64_t sta = ref[static_cast<std::size_t>(role::AttrType::Stamina)];
    ref[static_cast<std::size_t>(role::AttrType::MaxHp)] +=
        f.hp_per_stamina * sta + f.hp_base;
    ref[static_cast<std::size_t>(role::AttrType::MaxMp)] +=
        f.mp_per_intellect * inl + f.mp_base;
    ref[static_cast<std::size_t>(role::AttrType::Attack)] +=
        f.atk_per_strength * str + f.atk_base;
    ref[static_cast<std::size_t>(role::AttrType::Defense)] +=
        f.def_per_agility * agi + f.def_base;
    ref[static_cast<std::size_t>(role::AttrType::CritRate)] +=
        f.crit_per_agility * agi + f.crit_base;
    ref[static_cast<std::size_t>(role::AttrType::CritDamage)] +=
        f.critdmg_per_intellect * inl + f.critdmg_base;
    ref[static_cast<std::size_t>(role::AttrType::MoveSpeed)] +=
        f.movespeed_per_agility * agi + f.movespeed_base;

    // 2. 合并所有激活 Buff 的加法 + 乘法贡献
    std::array<std::int64_t, role::kAttrCount> fb{};
    auto it = by_char_.find(c->id);
    if (it != by_char_.end()) {
        for (const auto& inst : it->second) {
            const BuffDef& d = *inst.def;
            for (std::size_t i = 0; i < role::kAttrCount; ++i) {
                fb[i] += d.attr_modifiers[i];
                if (d.attr_multipliers[i] != 0.0) {
                    fb[i] += static_cast<std::int64_t>(
                        std::llround(d.attr_multipliers[i] * static_cast<double>(ref[i])));
                }
            }
        }
    }
    c->attrs.from_buff = fb;
    c->attrs.Recompute();
    ++stats_.recomputes;
}

void BuffSystem::RecomputeControlMask(role::CharacterId target) {
    std::uint8_t m = 0;
    auto it = by_char_.find(target);
    if (it != by_char_.end()) {
        for (const auto& inst : it->second) m |= inst.def->control_mask;
    }
    if (m == 0) {
        control_mask_.erase(target);
    } else {
        control_mask_[target] = m;
    }
}

EntityId BuffSystem::EntityOf(role::CharacterId target) const noexcept {
    auto it = char_to_entity_.find(target);
    return it == char_to_entity_.end() ? 0 : it->second;
}

// ---- Apply ----
core::Result<std::uint32_t> BuffSystem::Apply(role::CharacterId target,
                                              std::uint32_t buff_id,
                                              role::CharacterId source, core::TraceID trace,
                                              core::SteadyTime now) {
    // trace 在本路径不落事件（BuffApplied 事件体无 trace 字段，见 combat_events.h），
    // 显式丢弃以免 -Wunused-parameter。
    (void)trace;
    const BuffDef* d = defs_.Find(buff_id);
    if (d == nullptr) return Fail<std::uint32_t>(ErrorCode::NOT_FOUND, "unknown buff id");
    role::Character* c = roles_.Find(target);
    if (c == nullptr) return Fail<std::uint32_t>(ErrorCode::NOT_FOUND, "no such character");

    auto& vec = by_char_[target];

    // 已有同 id 实例？（下标统一用 size_t：早先用 int + -1 哨兵，每次下标访问都会
    // 触发 int→size_type 的 -Wsign-conversion。）
    constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);
    std::size_t exist_idx = kNoIndex;
    for (std::size_t i = 0; i < vec.size(); ++i) {
        if (vec[i].def->id == buff_id) {
            exist_idx = i;
            break;
        }
    }

    if (exist_idx != kNoIndex) {
        switch (d->stack_rule) {
            case StackRule::None:
                vec[exist_idx].expire_at =
                    (d->duration_ms == 0) ? kForever : now + Ms(d->duration_ms);
                return core::Result<std::uint32_t>::Ok(vec[exist_idx].stacks);
            case StackRule::Refresh:
                vec[exist_idx].stacks =
                    static_cast<std::uint16_t>(std::min<std::uint32_t>(
                        vec[exist_idx].stacks + 1, d->max_stacks));
                vec[exist_idx].expire_at =
                    (d->duration_ms == 0) ? kForever : now + Ms(d->duration_ms);
                return core::Result<std::uint32_t>::Ok(vec[exist_idx].stacks);
            case StackRule::Independent: {
                // 已达层数上限：顶掉最旧实例（首个），再新增
                std::size_t cnt = 0;
                for (const auto& inst : vec)
                    if (inst.def->id == buff_id) ++cnt;
                if (cnt >= d->max_stacks) {
                    // iterator + n 的形参是 ptrdiff_t，size_t 需显式转换
                    vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(exist_idx));
                    RecomputeFromBuff(c);
                    RecomputeControlMask(target);
                }
                break;  // 继续新增实例
            }
        }
    }

    // 新增实例（槽位上限）
    if (vec.size() >= kMaxBuffsPerChar) {
        return Fail<std::uint32_t>(ErrorCode::BUSY, "buff slots exhausted");
    }
    BuffInstance inst{};
    inst.def = d;
    inst.owner = target;
    inst.source = source;
    inst.stacks = 1;
    inst.shield_remaining = d->shield_value;
    inst.expire_at = (d->duration_ms == 0) ? kForever : now + Ms(d->duration_ms);
    inst.next_tick_at = (d->tick_interval_ms == 0) ? kForever : now + Ms(d->tick_interval_ms);
    vec.push_back(inst);

    RecomputeFromBuff(c);
    if (d->control_mask != 0) RecomputeControlMask(target);
    ++stats_.applies;

    if (bus_ != nullptr) {
        combat::BuffApplied ev{};
        ev.caster = EntityOf(source);
        ev.target = EntityOf(target);
        ev.buff_id = buff_id;
        ev.duration_ms = d->duration_ms;
        ev.stacks = inst.stacks;
        (void)bus_->Publish(ev);
    }
    return core::Result<std::uint32_t>::Ok(inst.stacks);
}

// ---- Remove ----
core::Result<void> BuffSystem::Remove(role::CharacterId target, std::uint32_t buff_id,
                                      RemoveReason reason, core::TraceID trace) {
    auto it = by_char_.find(target);
    if (it == by_char_.end()) return core::Result<void>::Ok();  // 幂等
    auto& vec = it->second;
    const std::size_t before = vec.size();
    // 倒序 swap-erase 同 id 的全部实例
    for (std::size_t i = vec.size(); i-- > 0;) {
        if (vec[i].def->id == buff_id) {
            const std::uint32_t removed_id = vec[i].def->id;
            vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(i));
            if (bus_ != nullptr) {
                combat::BuffRemoved ev{};
                ev.target = EntityOf(target);
                ev.buff_id = removed_id;
                ev.reason = static_cast<std::uint8_t>(reason);
                ev.stacks = 1;
                ev.trace = trace;
                (void)bus_->Publish(ev);
            }
        }
    }
    if (vec.size() == before) return core::Result<void>::Ok();  // 无匹配
    if (vec.empty()) by_char_.erase(it);
    role::Character* c = roles_.Find(target);
    if (c != nullptr) RecomputeFromBuff(c);
    RecomputeControlMask(target);
    return core::Result<void>::Ok();
}

// ---- Dispel ----
core::Result<void> BuffSystem::Dispel(role::CharacterId target, std::uint32_t buff_id,
                                      core::TraceID trace) {
    const BuffDef* d = defs_.Find(buff_id);
    if (d == nullptr) return Fail<void>(ErrorCode::NOT_FOUND, "unknown buff id");
    if (!d->dispellable) {
        return Fail<void>(ErrorCode::INVALID_ARGUMENT, "buff not dispellable");
    }
    auto it = by_char_.find(target);
    if (it == by_char_.end()) return core::Result<void>::Ok();
    auto& vec = it->second;
    const std::size_t before = vec.size();
    for (std::size_t i = vec.size(); i-- > 0;) {
        if (vec[i].def->id == buff_id) {
            const std::uint32_t removed_id = vec[i].def->id;
            vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(i));
            if (bus_ != nullptr) {
                combat::BuffDispelled ev{};
                ev.target = EntityOf(target);
                ev.buff_id = removed_id;
                ev.trace = trace;
                (void)bus_->Publish(ev);
            }
        }
    }
    if (vec.size() == before) return core::Result<void>::Ok();
    if (vec.empty()) by_char_.erase(it);
    role::Character* c = roles_.Find(target);
    if (c != nullptr) RecomputeFromBuff(c);
    RecomputeControlMask(target);
    ++stats_.dispels;
    return core::Result<void>::Ok();
}

// ---- Tick ----
void BuffSystem::Tick(const SceneContext& ctx) {
    const core::SteadyTime now = ctx.now;
    const core::TraceID trace = 0;

    std::vector<role::CharacterId> empty_keys;
    for (auto& kv : by_char_) {
        const role::CharacterId target = kv.first;
        auto& vec = kv.second;
        bool removed_any = false;
        role::Character* c = roles_.Find(target);

        for (std::size_t i = vec.size(); i-- > 0;) {
            BuffInstance& inst = vec[i];
            const BuffDef& d = *inst.def;

            // 到期
            if (now >= inst.expire_at) {
                const std::uint32_t expired_id = d.id;
                vec.erase(vec.begin() + static_cast<std::ptrdiff_t>(i));
                removed_any = true;
                ++stats_.expires;
                if (bus_ != nullptr) {
                    combat::BuffExpired ev{};
                    ev.target = EntityOf(target);
                    ev.buff_id = expired_id;
                    ev.trace = trace;
                    (void)bus_->Publish(ev);
                }
                continue;
            }

            // 周期结算（DOT/HOT）
            if (now >= inst.next_tick_at && d.tick_effect.amount != 0 && dmg_ != nullptr &&
                c != nullptr) {
                const EntityId src = EntityOf(inst.source);
                const EntityId tgt = EntityOf(target);
                if (d.tick_effect.is_heal) {
                    combat::HealRequest hr{};
                    hr.source = src;
                    hr.target = tgt;
                    hr.base_amount = d.tick_effect.amount * inst.stacks;
                    hr.coefficient = 0.0f;
                    hr.can_crit = d.tick_effect.can_crit;
                    hr.trace = trace;
                    (void)dmg_->ApplyHeal(hr, ctx);
                } else {
                    combat::DamageRequest dr{};
                    dr.source = src;
                    dr.target = tgt;
                    dr.school = static_cast<combat::DamageSchool>(d.tick_effect.school);
                    dr.base_amount = d.tick_effect.amount * inst.stacks;
                    dr.coefficient = 0.0f;
                    dr.can_crit = d.tick_effect.can_crit;
                    dr.trace = trace;
                    (void)dmg_->ApplyDamage(dr, ctx);
                }
                inst.next_tick_at = inst.next_tick_at + Ms(d.tick_interval_ms);
                ++stats_.ticks;
            }
        }

        if (removed_any) {
            if (vec.empty()) empty_keys.push_back(target);
            if (c != nullptr) RecomputeFromBuff(c);
            RecomputeControlMask(target);
        }
    }

    for (const auto k : empty_keys) by_char_.erase(k);
}

// ---- OnDeath ----
void BuffSystem::OnDeath(role::CharacterId target, core::TraceID trace) {
    auto it = by_char_.find(target);
    if (it == by_char_.end()) return;
    for (const auto& inst : it->second) {
        if (bus_ != nullptr) {
            combat::BuffRemoved ev{};
            ev.target = EntityOf(target);
            ev.buff_id = inst.def->id;
            ev.reason = static_cast<std::uint8_t>(RemoveReason::Death);
            // 事件字段是 uint8_t、实例层数是 uint16_t：饱和截断，避免静默回绕
            // （层数 >255 时上报 255，而不是 layers&0xFF）
            ev.stacks = static_cast<std::uint8_t>(std::min<std::uint16_t>(inst.stacks, 255));
            ev.trace = trace;
            (void)bus_->Publish(ev);
        }
    }
    by_char_.erase(it);
    control_mask_.erase(target);
    role::Character* c = roles_.Find(target);
    if (c != nullptr) {
        c->attrs.from_buff = {};
        c->attrs.Recompute();
    }
}

// ---- 只读访问 ----
const std::vector<BuffInstance>* BuffSystem::BuffsOf(role::CharacterId target) const noexcept {
    auto it = by_char_.find(target);
    return it == by_char_.end() ? nullptr : &it->second;
}

std::size_t BuffSystem::ActiveBuffCount(role::CharacterId target) const noexcept {
    auto it = by_char_.find(target);
    return it == by_char_.end() ? 0 : it->second.size();
}

std::uint8_t BuffSystem::ControlMask(role::CharacterId target) const noexcept {
    auto it = control_mask_.find(target);
    return it == control_mask_.end() ? 0 : it->second;
}

}  // namespace mmo::game::combat::buff
