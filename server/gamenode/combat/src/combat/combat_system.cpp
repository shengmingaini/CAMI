/// TASK-024 · CombatSystem 实现（§7 / §8 / §15 / §21 / §24）。
///
/// 热路径（§10 / §21 Forbidden）：Combat 阶段只做内存 / CPU / 本地接口调用；禁止
/// 外部存储 / 缓存 / 消息中间件 / 同步远程过程调用 / 文件 IO / 堆分配 / 线程 / 跨进程调用。
///
/// 红线（§24 静态扫描）：本文件源码与注释均不得出现后端依赖关键字（含外部存储 / 缓存 / 消息中间件 / 同步 RPC 等字样）。
/// 仇恨表定长 16（§15.2 / §19 禁止无界增长）；战斗状态用位标记（§8 / §21 Forbidden 散落 bool）。

#include "mmo/game/combat/combat_system.h"

#include <chrono>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/game/combat/buff/buff_def.h"  // buff::ControlFlag（控制标记位定义）

namespace mmo::game::combat {

namespace {
using core::Error;
using core::ErrorCode;

template <typename T>
core::Result<T> Fail(ErrorCode c, const char* msg) {
    return core::Result<T>::Fail(Error(c, msg, core::domain::kCore));
}

/// 控制类 Buff 标记 → CombatFlag 的位映射（§15.6 / §6 控制 Buff 影响移动与施法）。
inline void ApplyControlBit(CombatComponent::Flags& f, std::uint8_t mask,
                            buff::ControlFlag src, CombatFlag dst) noexcept {
    if (mask & static_cast<std::uint8_t>(src)) SetFlag(f, dst);
}

}  // namespace

// ===========================================================================
// 构造 / 装配
// ===========================================================================

CombatSystem::CombatSystem(SkillSystem& skills, DamageSystem& dmg, buff::BuffSystem& buffs,
                           role::RoleSystem& roles, EntityManager& entities, SceneId scene,
                           core::EventBus* bus)
    : skills_(skills), dmg_(dmg), buffs_(buffs), roles_(roles), entities_(entities),
      scene_(scene), bus_(bus) {}

void CombatSystem::BindEventBus(core::EventBus& bus) noexcept {
    bus_ = &bus;
    // 仇恨事件驱动（§8）：伤害 ×1.0、治疗 ×0.5，由 DamageSystem 发布的事件转入仇恨表。
    (void)bus.Subscribe<DamageEvent>([this](const DamageEvent& ev) { OnDamageEvent(ev); });
    (void)bus.Subscribe<HealEvent>([this](const HealEvent& ev) { OnHealEvent(ev); });
}

void CombatSystem::BindAvatar(EntityId entity, role::CharacterId character) noexcept {
    char_of_[entity] = character;
}

// ===========================================================================
// 查询辅助
// ===========================================================================

CombatEntity* CombatSystem::Find(EntityId self) noexcept {
    auto it = combatants_.find(self);
    return it == combatants_.end() ? nullptr : &it->second;
}
const CombatEntity* CombatSystem::Find(EntityId self) const noexcept {
    auto it = combatants_.find(self);
    return it == combatants_.end() ? nullptr : &it->second;
}

void CombatSystem::Touch(EntityId self, core::SteadyTime now) noexcept {
    CombatEntity* ce = Find(self);
    if (ce != nullptr) ce->last_combat_at = now;
}

void CombatSystem::SyncControlFlags(EntityId self) noexcept {
    CombatEntity* ce = Find(self);
    if (ce == nullptr) return;
    auto it = char_of_.find(self);
    if (it == char_of_.end()) return;
    const std::uint8_t mask = buffs_.ControlMask(it->second);
    // 先清 4 个控制位，再按 Buff 控制标记置位（避免残留）
    ClearFlag(ce->flags, CombatFlag::Stunned);
    ClearFlag(ce->flags, CombatFlag::Rooted);
    ClearFlag(ce->flags, CombatFlag::Silenced);
    ClearFlag(ce->flags, CombatFlag::Invulnerable);
    ApplyControlBit(ce->flags, mask, buff::ControlFlag::Stunned, CombatFlag::Stunned);
    ApplyControlBit(ce->flags, mask, buff::ControlFlag::Rooted, CombatFlag::Rooted);
    ApplyControlBit(ce->flags, mask, buff::ControlFlag::Silenced, CombatFlag::Silenced);
    ApplyControlBit(ce->flags, mask, buff::ControlFlag::Invulnerable, CombatFlag::Invulnerable);
}

// ===========================================================================
// 战斗状态机（§15）
// ===========================================================================

core::Result<void> CombatSystem::EnterCombat(EntityId self, EntityId enemy,
                                             core::TraceID /*trace*/) {
    if (self == 0 || enemy == 0) return Fail<void>(ErrorCode::NOT_FOUND, "invalid entity");
    CombatEntity& ce = combatants_[self];
    ce.id = self;
    SetFlag(ce.flags, CombatFlag::InCombat);
    ce.target = enemy;
    Touch(self, now_);

    // 互殴绑定：敌手也进入战斗并指向 self（双向 InCombat）
    CombatEntity& oe = combatants_[enemy];
    oe.id = enemy;
    SetFlag(oe.flags, CombatFlag::InCombat);
    oe.target = self;
    Touch(enemy, now_);

    ++stats_.enters;
    return core::Result<void>::Ok();
}

core::Result<void> CombatSystem::LeaveCombat(EntityId self, LeaveCombatReason reason,
                                             core::TraceID /*trace*/) {
    CombatEntity* ce = Find(self);
    if (ce == nullptr) return Fail<void>(ErrorCode::NOT_FOUND, "entity not in combat");
    ClearFlag(ce->flags, CombatFlag::InCombat);
    ClearFlag(ce->flags, CombatFlag::Casting);  // 脱战打断读条
    ce->threat.Clear();
    ce->target = 0;
    // 注：普通脱战（Timeout/Manual/ZoneOut）不清 Buff（§19）；Death 由 OnDeath 处理清 Buff。
    (void)reason;
    ++stats_.leaves;
    return core::Result<void>::Ok();
}

core::Result<CastResult> CombatSystem::CastSkill(const CastRequest& req, const SceneContext& ctx) {
    if (req.caster == 0) return Fail<CastResult>(ErrorCode::NOT_FOUND, "invalid caster");
    CombatEntity& ce = combatants_[req.caster];  // 懒创建：施法状态始终可追踪
    ce.id = req.caster;
    auto r = skills_.TryCast(req, ctx);
    if (r.HasValue()) {
        ++stats_.casts;
        // 读条标志跟随 SkillSystem 实际状态（瞬发技能不会置 Casting）
        if (skills_.CastingOf(req.caster) == CastingState::Casting) {
            SetFlag(ce.flags, CombatFlag::Casting);
        }
        Touch(req.caster, now_);
    }
    return r;
}

core::Result<void> CombatSystem::Interrupt(EntityId caster, InterruptReason reason,
                                           core::TraceID trace) {
    CombatEntity* ce = Find(caster);
    if (ce == nullptr) return Fail<void>(ErrorCode::NOT_FOUND, "invalid caster");
    auto r = skills_.InterruptCasting(caster, reason, trace);  // 内部发布 SkillInterrupted
    if (r.HasValue()) {
        ClearFlag(ce->flags, CombatFlag::Casting);
        ++stats_.interrupts;
    }
    return r;
}

core::Result<void> CombatSystem::Update(const SceneContext& ctx) {
    now_ = ctx.now;
    for (auto& kv : combatants_) {
        CombatEntity& ce = kv.second;
        if (!TestFlag(ce.flags, CombatFlag::InCombat)) continue;
        // 脱战计时：最近一次战斗行为满 leave_ms_ 即强制脱战（§8）
        if (ce.last_combat_at != core::SteadyTime{} &&
            (now_ - ce.last_combat_at) >= leave_ms_) {
            (void)LeaveCombat(ce.id, LeaveCombatReason::Timeout, 0);
            ++stats_.leave_timeout;
        } else {
            // 同步控制类 Buff → 战斗标志（影响移动 / 施法，§6）
            SyncControlFlags(ce.id);
        }
    }
    return core::Result<void>::Ok();
}

core::Result<void> CombatSystem::OnDeath(EntityId self, EntityId killer,
                                         core::TraceID trace) {
    (void)killer;
    CombatEntity* ce = Find(self);
    if (ce == nullptr) return Fail<void>(ErrorCode::NOT_FOUND, "entity not in combat");
    ClearFlag(ce->flags, CombatFlag::InCombat);
    ClearFlag(ce->flags, CombatFlag::Casting);
    SetFlag(ce->flags, CombatFlag::Dead);
    ce->threat.Clear();  // 死亡清仇恨（§19 禁止残留）
    ce->target = 0;

    // 死亡清 Buff（经 BuffSystem 接口，§15.9 / §27.3 禁直接改内部数据）
    auto it = char_of_.find(self);
    if (it != char_of_.end()) (void)buffs_.OnDeath(it->second, trace);

    ++stats_.deaths;
    // 注意：EntityDied 由 DamageSystem 在「由生到死」跃迁瞬间统一发布（HP 权威、只发一次，
    // 满足 §19「只发一次事件」），本方法不重复发布。
    return core::Result<void>::Ok();
}

// ===========================================================================
// 观测
// ===========================================================================

bool CombatSystem::HasFlag(EntityId self, CombatFlag flag) const noexcept {
    const CombatEntity* ce = Find(self);
    return ce != nullptr && TestFlag(ce->flags, flag);
}

std::optional<EntityId> CombatSystem::ThreatTop(EntityId self) const noexcept {
    const CombatEntity* ce = Find(self);
    if (ce == nullptr) return std::nullopt;
    return ce->threat.Top();
}

CombatStats CombatSystem::Stats() const noexcept {
    CombatStats s = stats_;
    s.in_combat_entities = 0;
    for (const auto& kv : combatants_) {
        if (TestFlag(kv.second.flags, CombatFlag::InCombat)) ++s.in_combat_entities;
    }
    return s;
}

// ===========================================================================
// 仇恨事件驱动（§8 / §15.4 / §15.5）
// ===========================================================================

void CombatSystem::OnDamageEvent(const DamageEvent& ev) noexcept {
    CombatEntity* ce = Find(ev.target);
    if (ce == nullptr) return;  // 目标未进入战斗 → 不累积仇恨
    ce->threat.Add(ev.caster, static_cast<std::int64_t>(ev.amount));  // 伤害 ×1.0
    ++stats_.threat_updates;
}

void CombatSystem::OnHealEvent(const HealEvent& ev) noexcept {
    CombatEntity* ce = Find(ev.target);
    if (ce == nullptr) return;
    ce->threat.Add(ev.caster,
                   static_cast<std::int64_t>(static_cast<double>(ev.amount) * 0.5));  // 治疗 ×0.5
    ++stats_.threat_updates;
}

}  // namespace mmo::game::combat
