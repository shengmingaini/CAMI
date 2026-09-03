// server/gamenode/combat/src/skill/skill_system.cpp — TASK-021 §15.4 / §15.5 / §15.6 / §15.7 / §15.8
//
// 施法状态机（§8）：Idle → Casting(读条) → Resolved(结算) → Cooldown → Idle。
// 关键不变式（§15.7 / §17 #5 / §19）：
//   · 资源（mana/hp）在施法**开始**扣除，打断**不退**（§21 Forbidden）。
//   · 冷却在施法**开始**即设置（瞬发与读条一致），因此客户端重复发包第二次必被冷却拦截，
//     不会重复结算。
//   · AOE 目标选取走 AOI 局部查询（§10 禁止全扫）。
//   · 飞行物按速度手动欧拉推进（§15.8 的 MovementSystem 全量管道联动留待 TASK-022）。

#include "mmo/game/combat/skill/skill_system.h"

#include <cmath>

#include "mmo/core/error/error_code.h"
#include "mmo/game/entity/entity.h"
#include "mmo/game/movement/movement_state.h"
#include "mmo/game/role/attribute.h"

namespace mmo::game::combat {
namespace {

using namespace mmo::game;
using namespace mmo::core;

/// 锥形内判定（xz 平面，yaw=0 朝 +Z 与 MovementSystem 约定一致）。
bool InCone(const Position& origin, float yaw, const Position& target, float cone_deg) {
    const float dx = target.x - origin.x;
    const float dz = target.z - origin.z;
    const float len = std::sqrt(dx * dx + dz * dz);
    if (len < 1e-4f) return true;  // 重合视为命中
    const float fx = std::sin(yaw);
    const float fz = std::cos(yaw);
    const float dot = (dx * fx + dz * fz) / len;
    const float cos_half = std::cos(cone_deg * 0.5f * (3.141592653589793194f / 180.0f));
    return dot >= cos_half;
}

}  // namespace

SkillSystem::SkillSystem(role::RoleSystem& roles, EntityManager& entities, aoi::IAoi* aoi,
                         SceneId scene, NodeId owner, core::EventBus* bus)
    : roles_(roles), entities_(entities), aoi_(aoi), scene_id_(scene),
      owner_node_(owner), bus_(bus) {}

core::Result<void> SkillSystem::LoadSkills(std::string_view json_text, const BuffRegistry& buffs) {
    auto r = registry_.Load(json_text, buffs);
    if (!r) return core::Result<void>::Fail(r.Err());
    cd_.SetSkillCount(registry_.Size());
    return core::Result<void>::Ok();
}

core::Result<void> SkillSystem::LoadSkillsFromDir(std::string_view dir, const BuffRegistry& buffs) {
    auto r = registry_.LoadFromDir(dir, buffs);
    if (!r) return core::Result<void>::Fail(r.Err());
    cd_.SetSkillCount(registry_.Size());
    return core::Result<void>::Ok();
}

void SkillSystem::BindAvatar(EntityId entity, role::CharacterId character) noexcept {
    avatar_of_[entity] = character;
}
void SkillSystem::UnbindAvatar(EntityId entity) noexcept {
    avatar_of_.erase(entity);
    silenced_.erase(entity);
    active_casts_.erase(entity);
    cd_.Reset(entity);
}

void SkillSystem::SetSilenced(EntityId caster, bool v) noexcept { silenced_[caster] = v; }
bool SkillSystem::IsSilenced(EntityId caster) const noexcept {
    auto it = silenced_.find(caster);
    return it != silenced_.end() && it->second;
}

const SkillDef* SkillSystem::ResolveDef(SkillId skill, std::uint32_t& out_index) const noexcept {
    if (cache_skill_id_ == skill) {
        out_index = cache_idx_;
    } else {
        const SkillDef* d = registry_.Find(skill);
        if (d == nullptr) {
            out_index = static_cast<std::uint32_t>(registry_.Size());
            return nullptr;
        }
        out_index = d->index_;
        cache_skill_id_ = skill;
        cache_idx_ = out_index;
    }
    return registry_.DefByIndex(out_index);
}

role::CharacterId SkillSystem::AvatarOf(EntityId e) const noexcept {
    auto it = avatar_of_.find(e);
    return it == avatar_of_.end() ? 0 : it->second;
}

std::vector<EntityId> SkillSystem::GatherAoe(EntityId caster, const SkillDef& def) const {
    std::vector<EntityId> out;
    const Entity* ce = entities_.Find(caster);
    if (ce == nullptr || aoi_ == nullptr) return out;
    std::vector<EntityId> visible;
    if (!aoi_->QueryVisible(caster, visible).HasValue()) return out;
    for (EntityId eid : visible) {
        if (eid == caster) continue;
        const Entity* e = entities_.Find(eid);
        if (e == nullptr) continue;
        if (e->Type() == ce->Type()) continue;  // 仅敌对
        const float d = movement::Distance3(ce->Pos(), e->Pos());
        if (d > def.radius) continue;
        if (def.target_type == TargetType::AoeCone &&
            !InCone(ce->Pos(), ce->Pos().yaw, e->Pos(), def.cone_deg)) {
            continue;
        }
        out.push_back(eid);
    }
    return out;
}

void SkillSystem::ApplyEffects(EntityId caster,
                               const std::vector<EffectDef>& effects,
                               const std::vector<EntityId>& targets, core::TraceID trace) {
    const role::CharacterId caster_char = AvatarOf(caster);
    const role::Character* cp =
        (caster_char != 0) ? roles_.Find(caster_char) : nullptr;
    const std::int64_t power =
        (cp != nullptr) ? cp->attrs.Total(role::AttrType::Attack) : 0;
    const float power_f = static_cast<float>(power);

    for (EntityId t : targets) {
        const role::CharacterId tchar = AvatarOf(t);
        for (const EffectDef& eff : effects) {
            switch (eff.type) {
                case EffectType::Damage: {
                    const std::int64_t amount =
                        static_cast<std::int64_t>(eff.base + eff.coeff * power_f);
                    if (bus_ != nullptr) {
                        // 单跳伤害设计上 < 2^31（见 combat_events.h）；DamageEvent.amount 为 int32，
                        // 显式限定消除 brace-init 的 narrowing/转换告警。权威扣血走 ModifyHp 全量 int64。
                        (void)bus_->Publish(DamageEvent{
                            caster, t, static_cast<std::int32_t>(amount),
                            PackFx(eff.school, eff.can_crit), trace});
                    }
                    if (tchar != 0) (void)roles_.ModifyHp(tchar, -amount, trace);
                    break;
                }
                case EffectType::Heal: {
                    const std::int64_t amount =
                        static_cast<std::int64_t>(eff.base + eff.coeff * power_f);
                    if (bus_ != nullptr) {
                        (void)bus_->Publish(HealEvent{caster, t, amount, trace});
                    }
                    if (tchar != 0) (void)roles_.ModifyHp(tchar, +amount, trace);
                    break;
                }
                case EffectType::ApplyBuff: {
                    if (bus_ != nullptr) {
                        (void)bus_->Publish(BuffApplied{caster, t, eff.buff_id,
                                                  eff.duration_ms, eff.stacks});
                    }
                    break;  // 实际施加在 TASK-023
                }
                case EffectType::SpawnProjectile:
                    break;  // 由 target_type==Projectile 路径生成，效果列表里该条目忽略
            }
        }
    }
}

void SkillSystem::SpawnProjectile(const SkillDef& def, std::uint32_t skill_index,
                                  EntityId caster, EntityId target, core::TraceID trace) {
    Entity* ce = entities_.Find(caster);
    if (ce == nullptr) return;
    Entity* te = entities_.Find(target);

    float speed = 12.0f, hit_radius = 2.0f, max_distance = 40.0f;
    for (const EffectDef& e : def.effects) {
        if (e.type == EffectType::SpawnProjectile) {
            speed = e.speed;
            hit_radius = e.radius;
            max_distance = e.max_distance;
            break;
        }
    }

    auto cr = entities_.Create(EntityType::Projectile, scene_id_, ce->Pos());
    if (!cr.HasValue()) return;  // 实体上限（极少见）：放弃本次飞行物，不阻塞主流程
    const EntityId pid = cr.Value()->Id();

    Projectile p;
    p.id = pid;
    p.caster = caster;
    p.target = target;
    p.skill = def.id;
    p.skill_index = skill_index;
    p.trace = trace;
    p.pos = ce->Pos();
    p.max_distance = max_distance;
    p.hit_radius = hit_radius;
    p.effects = def.effects;

    Position tp = (te != nullptr) ? te->Pos() : Position{};
    movement::Vec3 dir{tp.x - ce->Pos().x, tp.y - ce->Pos().y, tp.z - ce->Pos().z};
    const float len = movement::Vec3Length(dir);
    if (len < 1e-4f) {
        dir = movement::Vec3{0.0f, 0.0f, 1.0f};
    } else {
        dir = movement::Vec3{dir.x / len, dir.y / len, dir.z / len};
    }
    p.vel = movement::Vec3{dir.x * speed, dir.y * speed, dir.z * speed};
    projectiles_.push_back(std::move(p));
}

void SkillSystem::AdvanceProjectiles(float dt_sec) {
    if (projectiles_.empty()) return;
    std::vector<std::size_t> remove_idx;
    for (std::size_t i = 0; i < projectiles_.size(); ++i) {
        Projectile& p = projectiles_[i];
        Entity* pe = entities_.Find(p.id);
        if (pe == nullptr) { remove_idx.push_back(i); continue; }  // 飞行物实体消失

        const float speed = movement::Vec3Length(p.vel);
        p.traveled += speed * dt_sec;

        Position np;
        np.x = p.pos.x + p.vel.x * dt_sec;
        np.y = p.pos.y + p.vel.y * dt_sec;
        np.z = p.pos.z + p.vel.z * dt_sec;
        np.yaw = p.pos.yaw;
        p.pos = np;
        pe->SetPos(np);

        Entity* te = entities_.Find(p.target);
        if (te == nullptr) { remove_idx.push_back(i); continue; }  // 目标中途死亡 -> 飞行物消失（§19）

        const float d = movement::Distance3(np, te->Pos());
        if (d <= p.hit_radius) {
            ApplyEffects(p.caster, p.effects, {p.target}, p.trace);
            remove_idx.push_back(i);
            continue;
        }
        if (p.traveled >= p.max_distance) { remove_idx.push_back(i); continue; }
    }
    // 逆序删除，避免下标错位
    for (auto it = remove_idx.rbegin(); it != remove_idx.rend(); ++it) {
        Projectile& p = projectiles_[*it];
        (void)entities_.Destroy(p.id);
        projectiles_.erase(projectiles_.begin() + static_cast<std::ptrdiff_t>(*it));
    }
}

core::Result<CastResult> SkillSystem::TryCast(const CastRequest& req,
                                             [[maybe_unused]] const SceneContext& ctx) {
    std::uint32_t idx = 0;
    const SkillDef* def = ResolveDef(req.skill, idx);
    if (def == nullptr) return core::Result<CastResult>::Ok(CastResult::InvalidTarget);

    Entity* caster_e = entities_.Find(req.caster);
    if (caster_e == nullptr) return core::Result<CastResult>::Ok(CastResult::InvalidTarget);

    if (IsSilenced(req.caster)) return core::Result<CastResult>::Ok(CastResult::Silenced);

    // 本帧时间只取一次（热路径零额外时钟开销；§15.3），供冷却判定与到期写入共用。
    const std::uint64_t now = static_cast<std::uint64_t>(core::MonotonicClock::Now());
    if (IsOnCooldown(req.caster, req.skill, now)) return core::Result<CastResult>::Ok(CastResult::OnCooldown);

    const role::CharacterId caster_char = AvatarOf(req.caster);
    if (caster_char == 0) return core::Result<CastResult>::Ok(CastResult::InvalidTarget);
    role::Character* cp = roles_.Find(caster_char);
    if (cp == nullptr) return core::Result<CastResult>::Ok(CastResult::InvalidTarget);

    // 资源校验（不足即拒绝，未扣）
    if (def->mana_cost > 0 && cp->mp < def->mana_cost)
        return core::Result<CastResult>::Ok(CastResult::InsufficientResource);
    if (def->hp_cost > 0 && cp->hp < def->hp_cost)
        return core::Result<CastResult>::Ok(CastResult::InsufficientResource);

    // 目标选取
    std::vector<EntityId> targets;
    if (def->target_type == TargetType::Self) {
        targets.push_back(req.caster);
    } else if (def->target_type == TargetType::SingleTarget ||
               def->target_type == TargetType::Projectile) {
        if (req.target == 0) return core::Result<CastResult>::Ok(CastResult::NoTarget);
        Entity* te = entities_.Find(req.target);
        if (te == nullptr) return core::Result<CastResult>::Ok(CastResult::NoTarget);
        if (te->Type() == caster_e->Type())
            return core::Result<CastResult>::Ok(CastResult::InvalidTarget);
        const float d = movement::Distance3(caster_e->Pos(), te->Pos());
        if (d > def->range + 1e-3f) return core::Result<CastResult>::Ok(CastResult::OutOfRange);
        targets.push_back(req.target);
    } else {  // AoeCircle / AoeCone
        targets = GatherAoe(req.caster, *def);
        if (targets.empty()) return core::Result<CastResult>::Ok(CastResult::NoTarget);
    }

    // 扣费（施法开始；打断不退，§15.7）
    if (def->mana_cost > 0) (void)roles_.ModifyMp(caster_char, -def->mana_cost, req.trace);
    if (def->hp_cost > 0) (void)roles_.ModifyHp(caster_char, -def->hp_cost, req.trace);

    // 冷却施法开始即设置（§17 / §19）
    const std::int64_t cd_ns =
        static_cast<std::int64_t>(static_cast<double>(def->cooldown) * 1e9);
    const std::uint64_t expire = now + static_cast<std::uint64_t>(cd_ns);
    cd_.Set(req.caster, idx, expire);

    // 发布 SkillCast（读条开始即发布，供打断系统订阅，§15.5）
    const EntityId cast_target =
        (def->target_type == TargetType::Self) ? req.caster : req.target;
    if (bus_ != nullptr) (void)bus_->Publish(SkillCast{req.caster, req.skill, cast_target, req.trace});

    if (def->target_type == TargetType::Projectile) {
        SpawnProjectile(*def, idx, req.caster, req.target, req.trace);
        return core::Result<CastResult>::Ok(CastResult::Ok);
    }

    if (def->cast_time > 0.0f) {
        ActiveCast ac;
        ac.skill = req.skill;
        ac.skill_index = idx;
        ac.end_ns =
            core::MonotonicClock::Now() + static_cast<std::int64_t>(def->cast_time * 1e9f);
        ac.ttype = def->target_type;
        ac.targets = targets;
        ac.trace = req.trace;
        active_casts_[req.caster] = std::move(ac);
        return core::Result<CastResult>::Ok(CastResult::Ok);
    }

    // 瞬发：立即结算
    ApplyEffects(req.caster, def->effects, targets, req.trace);
    return core::Result<CastResult>::Ok(CastResult::Ok);
}

core::Result<void> SkillSystem::Update([[maybe_unused]] const SceneContext& ctx) {
    const core::SteadyNs now = core::MonotonicClock::Now();
    float dt_sec = 0.0f;
    if (last_tick_ns_ != 0) {
        const std::int64_t delta = now - last_tick_ns_;
        dt_sec = static_cast<float>(delta) / 1e9f;
        if (dt_sec < 0.0f) dt_sec = 0.0f;
    }
    last_tick_ns_ = now;

    // 完成读条
    std::vector<EntityId> done;
    for (const auto& kv : active_casts_) {
        if (now >= kv.second.end_ns) done.push_back(kv.first);
    }
    for (EntityId c : done) {
        auto it = active_casts_.find(c);
        if (it == active_casts_.end()) continue;
        ActiveCast ac = std::move(it->second);
        active_casts_.erase(it);
        const SkillDef* def = registry_.DefByIndex(ac.skill_index);
        if (def != nullptr) ApplyEffects(c, def->effects, ac.targets, ac.trace);
    }

    AdvanceProjectiles(dt_sec);
    return core::Result<void>::Ok();
}

core::Result<void> SkillSystem::InterruptCasting(EntityId caster, InterruptReason reason,
                                                core::TraceID trace) {
    auto it = active_casts_.find(caster);
    if (it == active_casts_.end()) {
        return core::Result<void>::Ok();  // 未在读条：幂等空操作
    }
    const ActiveCast ac = std::move(it->second);
    active_casts_.erase(it);
    if (bus_ != nullptr) (void)bus_->Publish(SkillInterrupted{caster, ac.skill, reason, trace});
    // 冷却已在施法开始设置（不取消）；资源已扣（不退，§15.7 / §21 Forbidden）。
    return core::Result<void>::Ok();
}

bool SkillSystem::IsOnCooldown(EntityId caster, SkillId skill, std::uint64_t now_ns) const noexcept {
    std::uint32_t idx = 0;
    const SkillDef* def = ResolveDef(skill, idx);
    if (def == nullptr) return false;
    return cd_.IsOnCooldown(caster, idx, now_ns);
}

core::DurationMs SkillSystem::CooldownRemaining(EntityId caster, SkillId skill,
                                                std::uint64_t now_ns) const noexcept {
    std::uint32_t idx = 0;
    const SkillDef* def = ResolveDef(skill, idx);
    if (def == nullptr) return core::DurationMs(0);
    return cd_.Remaining(caster, idx, now_ns);
}

bool SkillSystem::IsOnCooldown(EntityId caster, SkillId skill) const noexcept {
    return IsOnCooldown(caster, skill,
                        static_cast<std::uint64_t>(core::MonotonicClock::Now()));
}

core::DurationMs SkillSystem::CooldownRemaining(EntityId caster, SkillId skill) const noexcept {
    return CooldownRemaining(caster, skill,
                             static_cast<std::uint64_t>(core::MonotonicClock::Now()));
}

CastingState SkillSystem::CastingOf(EntityId caster) const noexcept {
    return active_casts_.find(caster) != active_casts_.end()
               ? CastingState::Casting
               : CastingState::Idle;
}

}  // namespace mmo::game::combat
