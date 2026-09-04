// server/gamenode/combat/src/damage/damage_system.cpp — TASK-022 §15.4 ~ §15.8
//
// 结算顺序（§20.2，固定，禁止调整）：
//   暴击判定 → 闪避判定 → 抗性减免 → 护盾吸收 → 扣 HP → 致死判定 → 发布事件
//
// 热路径红线（§10 / §21）：
//   · 禁止 MySQL / Redis / Kafka / 同步 gRPC / 文件 IO；
//   · 禁止堆分配（alloc_per_damage = 0）—— 总线事件 ≤32B 走 EventSlot 内联，
//     完整结算记录写预分配的环形缓冲，绝不进总线；
//   · 禁止 unordered_map 哈希查找（EntityId → CharacterId 走扁平数组 O(1)，§15.3）。
//
// 单写入者约束（§4 / §27.3）：HP 的写入**只经 RoleSystem::ModifyHp**，
// 本系统不直接改 Character::hp —— 那会破坏「角色数据唯一权威在 Role」的约定。

#include "mmo/game/combat/damage/damage_system.h"

#include <cmath>
#include <cstdint>
#include <limits>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/game/combat/skill/combat_events.h"  // DamageEvent / HealEvent（TASK-021 冻结事件）
#include "mmo/game/entity/entity_id.h"

namespace mmo::game::combat {
namespace {

constexpr std::string_view kDomain = "combat";

core::Error Fail(core::ErrorCode code, std::string_view msg) {
    return core::Error(code, msg, kDomain);
}

/// 属性 → 公式入参（§15.2 公式层不依赖 role，映射留在本层）。
struct CombatParams {
    std::int64_t attack{0};
    std::int64_t defense{0};
    std::int64_t agility{0};
    std::int64_t crit_rate{0};       // 万分比
    std::int64_t crit_damage{0};     // 万分比（0 = 未配置，用 formula.crit_damage_default）
    std::int64_t max_hp{0};
};

CombatParams ReadParams(const role::AttributeSet& a) noexcept {
    using role::AttrType;
    CombatParams p;
    p.attack = a.Total(AttrType::Attack);
    p.defense = a.Total(AttrType::Defense);
    p.agility = a.Total(AttrType::Agility);
    p.crit_rate = a.Total(AttrType::CritRate);
    p.crit_damage = a.Total(AttrType::CritDamage);
    p.max_hp = a.Total(AttrType::MaxHp);
    return p;
}

}  // namespace

// ============================================================================
// 统计派生指标
// ============================================================================

std::int64_t DamageStats::CritRateBp() const noexcept {
    if (damage_events == 0) return 0;
    return static_cast<std::int64_t>(crit_count * 10000 / damage_events);
}

std::int64_t DamageStats::DodgeRateBp() const noexcept {
    if (damage_events == 0) return 0;
    return static_cast<std::int64_t>(dodge_count * 10000 / damage_events);
}

std::int64_t DamageStats::AvgDamageX10000() const noexcept {
    if (damage_events == 0) return 0;
    return static_cast<std::int64_t>(total_final * 10000 / damage_events);
}

// ============================================================================
// 构造与装配
// ============================================================================

DamageSystem::DamageSystem(const DamageFormula& formula, role::RoleSystem& roles,
                           EntityManager& entities, SceneId scene, core::EventBus* bus)
    : formula_(formula),
      roles_(roles),
      entities_(entities),
      scene_(scene),
      events_(bus),
      prng_(0) {
    // 环形缓冲**构造期一次性分配**：运行期采样写入零分配（§21 alloc_per_damage = 0）。
    const std::size_t cap = formula_.log_ring_capacity > 0
                                ? static_cast<std::size_t>(formula_.log_ring_capacity)
                                : 1u;
    ring_.resize(cap);
    // 按 (SceneId, Tick 0, 序号 0) 播种，保证「未显式 Reseed 也有确定性序列」而非随机态。
    prng_.Seed(scene_, 0, 0);
    // last_tick_ 用「不可能出现的 Tick 号」做哨兵：第一帧的 Tick 号可能是 0，
    // 若初值取 0 则 TickSamplingBudget 的「新 Tick 检测」不触发，首帧采样预算恒为 0。
    last_tick_ = std::numeric_limits<std::uint64_t>::max();
    sample_budget_ = 0;
}

void DamageSystem::EnsureAvatarSlot(std::uint32_t index) noexcept {
    if (index >= avatar_char_.size()) {
        avatar_char_.resize(static_cast<std::size_t>(index) + 1, role::kInvalidCharacterId);
        avatar_gen_.resize(static_cast<std::size_t>(index) + 1, 0u);
    }
}

void DamageSystem::BindAvatar(EntityId entity, role::CharacterId character) noexcept {
    const std::uint32_t idx = EntityIndex(entity);
    EnsureAvatarSlot(idx);
    avatar_char_[idx] = character;
    avatar_gen_[idx] = EntityGeneration(entity);
}

void DamageSystem::UnbindAvatar(EntityId entity) noexcept {
    const std::uint32_t idx = EntityIndex(entity);
    if (idx >= avatar_char_.size()) return;
    // 世代不匹配 → 该槽已被新实体复用，禁止误清（TASK-011 ABA 防护）。
    if (avatar_gen_[idx] != EntityGeneration(entity)) return;
    avatar_char_[idx] = role::kInvalidCharacterId;
    avatar_gen_[idx] = 0u;
}

role::CharacterId DamageSystem::AvatarOf(EntityId entity) const noexcept {
    const std::uint32_t idx = EntityIndex(entity);
    if (idx >= avatar_char_.size()) return role::kInvalidCharacterId;
    if (avatar_gen_[idx] != EntityGeneration(entity)) return role::kInvalidCharacterId;
    return avatar_char_[idx];
}

void DamageSystem::Reseed(SceneId scene, std::uint64_t tick, std::uint64_t seq) noexcept {
    scene_ = scene;
    prng_.Seed(scene, tick, seq);
}

// ============================================================================
// 纯函数结算（§7 / §15.2）
// ============================================================================

DamageResult DamageSystem::ComputeDamage(const DamageRequest& req,
                                         const role::AttributeSet& attacker,
                                         const role::AttributeSet& defender,
                                         const DamageRolls& rolls) const noexcept {
    DamageResult r{};
    const CombatParams atk = ReadParams(attacker);
    const CombatParams def = ReadParams(defender);

    // ---- raw = base_amount + coefficient * AttackPower（§8）----
    const double scaled = static_cast<double>(req.coefficient) * static_cast<double>(atk.attack);
    // 非有限系数在 ApplyDamage 入口已被拒绝；纯函数路径再做一次饱和防御，
    // 绝不产生 NaN 参与整数转换（§19）。
    const std::int64_t from_coeff =
        std::isfinite(scaled) ? static_cast<std::int64_t>(scaled) : 0;
    std::int64_t raw = req.base_amount + from_coeff;
    raw = ClampRaw(raw, formula_);
    r.raw = raw;

    // ---- 【1】暴击判定（§20.2 顺序：暴击 → 闪避 → 抗性 → 护盾 → 扣血 → 致死 → 事件）----
    const bool crit_allowed = req.can_crit &&
                              (req.school != DamageSchool::TrueDamage || formula_.true_damage_can_crit);
    if (crit_allowed) {
        std::int64_t crit_damage = atk.crit_damage;
        if (crit_damage <= 0) crit_damage = formula_.crit_damage_default;
        bool is_crit = false;
        raw = ApplyCrit(raw, atk.crit_rate, crit_damage, rolls.crit, formula_, &is_crit);
        r.is_crit = is_crit;
    }

    // ---- 【2】闪避判定 ----
    const bool dodge_allowed = req.can_be_dodged &&
                               (req.school != DamageSchool::TrueDamage || !formula_.true_damage_ignores_dodge);
    if (dodge_allowed) {
        const std::int64_t dodge_rate = DodgeRateOf(def.agility, formula_);
        if (IsDodged(dodge_rate, rolls.dodge)) {
            r.is_dodged = true;
            r.mitigated = 0;
            r.absorbed = 0;
            r.final_amount = 0;
            // remaining_hp 由 ApplyDamage 填（纯计算拿不到目标当前 HP）
            return r;
        }
    }

    // ---- 【3】抗性减免 ----
    if (req.school == DamageSchool::TrueDamage && formula_.true_damage_ignores_mitigation) {
        r.mitigated = raw;
    } else {
        r.mitigated = Mitigate(raw, def.defense, formula_);
    }

    // ---- 【4】护盾吸收（真实护盾值由 ApplyDamage 注入；纯函数路径无护盾来源）----
    std::int64_t absorbed = 0;
    bool blocked = false;
    r.final_amount = Absorb(r.mitigated, 0, formula_, &absorbed, &blocked);
    r.absorbed = absorbed;
    r.is_blocked = blocked;
    return r;
}

DamageResult DamageSystem::ComputeDamage(const DamageRequest& req,
                                         const role::AttributeSet& attacker,
                                         const role::AttributeSet& defender) noexcept {
    DamageRolls rolls{};
    rolls.crit = prng_.NextScaled(static_cast<std::uint32_t>(formula_.rate_scale));
    rolls.dodge = prng_.NextScaled(static_cast<std::uint32_t>(formula_.rate_scale));
    return ComputeDamage(req, attacker, defender, rolls);
}

// ============================================================================
// 采样日志（§15.8 / §20.6）
// ============================================================================

void DamageSystem::TickSamplingBudget(std::uint64_t tick_number) noexcept {
    if (tick_number != last_tick_) {
        // 新 Tick：按「每秒 N 条 / Tick 频率」重算预算，并把上一 Tick 的余额丢弃
        // （不累积，避免长时间无伤害后突然爆发式采样）。
        const std::int64_t per_tick = formula_.tick_rate_hz > 0
                                          ? formula_.log_samples_per_second / formula_.tick_rate_hz
                                          : 0;
        sample_budget_ = per_tick;
        last_tick_ = tick_number;
    }
}

std::span<const DamageRecord> DamageSystem::TakeSamples() noexcept {
    if (ring_count_ == 0) return {};
    // 环形缓冲的未消费区间可能绕回，这里只返回**连续的一段**；
    // 调用方高频取走即可（每秒 ~100 条，远小于容量）。
    const std::size_t start = read_pos_;
    std::size_t n = ring_count_;
    if (start + n > ring_.size()) n = ring_.size() - start;
    const std::span<const DamageRecord> view(ring_.data() + start, n);
    read_pos_ = (start + n) % ring_.size();
    ring_count_ -= n;
    return view;
}

// ============================================================================
// 结算并落状态（§7 / §15.6 / §15.7）
// ============================================================================

core::Result<DamageResult> DamageSystem::ApplyDamage(const DamageRequest& req,
                                                     const SceneContext& ctx, DamageRecord* out) {
    // ---- 入参校验（§19：NaN 系数 → INVALID_ARGUMENT）----
    if (!std::isfinite(req.coefficient)) {
        return core::Result<DamageResult>::Fail(
            Fail(core::ErrorCode::INVALID_ARGUMENT, "damage coefficient is NaN/Inf"));
    }
    if (req.base_amount < 0) {
        return core::Result<DamageResult>::Fail(
            Fail(core::ErrorCode::INVALID_ARGUMENT, "damage base_amount must be >= 0"));
    }

    // ---- 目标解析：实体存在性 + EntityId → CharacterId（扁平数组 O(1)，无哈希）----
    if (entities_.Find(req.target) == nullptr) {
        return core::Result<DamageResult>::Fail(
            Fail(core::ErrorCode::NOT_FOUND, "target entity not found"));
    }
    const role::CharacterId cid = AvatarOf(req.target);
    if (cid == role::kInvalidCharacterId) {
        return core::Result<DamageResult>::Fail(
            Fail(core::ErrorCode::NOT_FOUND, "target avatar not bound"));
    }
    role::Character* tc = roles_.Find(cid);
    if (tc == nullptr) {
        return core::Result<DamageResult>::Fail(
            Fail(core::ErrorCode::NOT_FOUND, "target character not found"));
    }
    // 已死亡目标：忽略，不产生负 HP、不改任何状态（§19）。
    if (tc->hp <= formula_.lethal_hp_threshold) {
        return core::Result<DamageResult>::Fail(
            Fail(core::ErrorCode::NOT_FOUND, "target already dead"));
    }

    // ---- 攻方属性（可选：无绑定时按 0 属性参与，仍允许结算）----
    static const role::AttributeSet kEmptyAttrs{};
    const role::CharacterId scid = AvatarOf(req.source);
    role::Character* sc = (scid != role::kInvalidCharacterId) ? roles_.Find(scid) : nullptr;
    const role::AttributeSet& atk_attrs = (sc != nullptr) ? sc->attrs : kEmptyAttrs;

    // ---- 随机（per-Scene 确定性 PRNG，§4 / §21）----
    DamageRolls rolls{};
    rolls.crit = prng_.NextScaled(static_cast<std::uint32_t>(formula_.rate_scale));
    rolls.dodge = prng_.NextScaled(static_cast<std::uint32_t>(formula_.rate_scale));

    DamageResult r = ComputeDamage(req, atk_attrs, tc->attrs, rolls);

    // ---- 【4】护盾吸收（真实护盾来自 TASK-023 注入的 IShieldSource，本任务只走接口）----
    const std::int64_t shield = (shields_ != nullptr) ? shields_->ShieldOf(req.target) : 0;
    std::int64_t absorbed = 0;
    bool blocked = false;
    r.final_amount = Absorb(r.mitigated, shield, formula_, &absorbed, &blocked);
    r.absorbed = absorbed;
    r.is_blocked = blocked;
    if (absorbed > 0 && shields_ != nullptr) shields_->ConsumeShield(req.target, absorbed);

    // ---- 【5】扣 HP（唯一写入口：RoleSystem::ModifyHp，§4 单写入者）----
    const std::int64_t hp_before = tc->hp;
    if (r.final_amount > 0) {
        const auto mv = roles_.ModifyHp(cid, -r.final_amount, req.trace);
        if (!mv.HasValue()) return core::Result<DamageResult>::Fail(mv.Err());
    }
    r.remaining_hp = tc->hp;

    // ---- 【6】致死判定：只在「由生到死」的跃迁瞬间置位，保证 EntityDied 只发一次 ----
    const bool was_alive = hp_before > formula_.lethal_hp_threshold;
    r.lethal = was_alive && tc->hp <= formula_.lethal_hp_threshold;

    // ---- 统计（§15.7）----
    ++stats_.damage_events;
    if (r.is_dodged) {
        ++stats_.dodge_count;
    } else {
        ++stats_.hit_count;
        if (r.is_crit) ++stats_.crit_count;
        if (r.is_blocked) ++stats_.blocked_count;
    }
    if (r.lethal) ++stats_.lethal_count;
    stats_.total_raw += static_cast<std::uint64_t>(r.raw);
    stats_.total_final += static_cast<std::uint64_t>(r.final_amount);
    stats_.total_absorbed += static_cast<std::uint64_t>(r.absorbed);

    // ---- 【7】发布事件（32B 内联，零分配）----
    if (events_ != nullptr) {
        DamageEvent ev{};
        ev.caster = req.source;
        ev.target = req.target;
        ev.amount = static_cast<std::int32_t>(r.final_amount);
        ev.packed = PackFx(static_cast<std::uint32_t>(req.school), r.is_crit);
        ev.trace = req.trace;
        (void)events_->Publish(ev);

        if (r.lethal) {
            EntityDied died{};
            died.entity = req.target;
            died.killer = req.source;
            died.trace = req.trace;
            (void)events_->Publish(died);
        }
    }

    // ---- 采样日志（§15.8：每秒 N 条，禁止每条全写）----
    TickSamplingBudget(ctx.tick_number);
    if (sample_budget_ > 0) {
        --sample_budget_;
        if (ring_count_ < ring_.size()) {
            DamageRecord& rec = ring_[ring_head_];
            rec.source = req.source;
            rec.target = req.target;
            rec.school = req.school;
            rec.result = r;
            rec.tick_number = ctx.tick_number;
            rec.trace = req.trace;
            ring_head_ = (ring_head_ + 1) % ring_.size();
            ++ring_count_;
            ++stats_.log_records;
        } else {
            ++stats_.log_dropped;  // 缓冲满：丢弃计数（不允许扩容 → 零分配）
        }
    }

    if (out != nullptr) {
        out->source = req.source;
        out->target = req.target;
        out->school = req.school;
        out->result = r;
        out->tick_number = ctx.tick_number;
        out->trace = req.trace;
    }
    return core::Result<DamageResult>::Ok(r);
}

core::Result<HealResult> DamageSystem::ApplyHeal(const HealRequest& req, const SceneContext& ctx) {
    if (!std::isfinite(req.coefficient)) {
        return core::Result<HealResult>::Fail(
            Fail(core::ErrorCode::INVALID_ARGUMENT, "heal coefficient is NaN/Inf"));
    }
    if (req.base_amount < 0) {
        return core::Result<HealResult>::Fail(
            Fail(core::ErrorCode::INVALID_ARGUMENT, "heal base_amount must be >= 0"));
    }

    if (entities_.Find(req.target) == nullptr) {
        return core::Result<HealResult>::Fail(
            Fail(core::ErrorCode::NOT_FOUND, "target entity not found"));
    }
    const role::CharacterId cid = AvatarOf(req.target);
    if (cid == role::kInvalidCharacterId) {
        return core::Result<HealResult>::Fail(
            Fail(core::ErrorCode::NOT_FOUND, "target avatar not bound"));
    }
    role::Character* tc = roles_.Find(cid);
    if (tc == nullptr) {
        return core::Result<HealResult>::Fail(
            Fail(core::ErrorCode::NOT_FOUND, "target character not found"));
    }
    // 死亡目标不可治疗（§19：不复活、不产生有效治疗）。
    if (tc->hp <= formula_.lethal_hp_threshold) {
        return core::Result<HealResult>::Fail(
            Fail(core::ErrorCode::NOT_FOUND, "target already dead"));
    }

    static const role::AttributeSet kEmptyAttrs{};
    const role::CharacterId scid = AvatarOf(req.source);
    role::Character* sc = (scid != role::kInvalidCharacterId) ? roles_.Find(scid) : nullptr;
    const role::AttributeSet& atk_attrs = (sc != nullptr) ? sc->attrs : kEmptyAttrs;

    // ---- raw = base + coefficient * Attack，暴击同伤害侧（§8 / §15.7）----
    const double scaled =
        static_cast<double>(req.coefficient) * static_cast<double>(atk_attrs.Total(role::AttrType::Attack));
    const std::int64_t from_coeff = std::isfinite(scaled) ? static_cast<std::int64_t>(scaled) : 0;
    std::int64_t raw = req.base_amount + from_coeff;
    raw = ClampRaw(raw, formula_);

    HealResult r{};
    r.is_crit = false;
    if (req.can_crit) {
        std::int64_t crit_rate = atk_attrs.Total(role::AttrType::CritRate);
        std::int64_t crit_damage = atk_attrs.Total(role::AttrType::CritDamage);
        if (crit_damage <= 0) crit_damage = formula_.crit_damage_default;
        bool crit = false;
        raw = ApplyCrit(raw, crit_rate, crit_damage,
                        prng_.NextScaled(static_cast<std::uint32_t>(formula_.rate_scale)), formula_,
                        &crit);
        r.is_crit = crit;
    }
    r.raw = raw;

    // ---- 钳制到 MaxHp（禁止溢出，§21）----
    const std::int64_t max_hp = tc->MaxHp();
    SettleHeal(raw, tc->hp, max_hp, &r.effective, &r.overheal);

    const bool full_hp_noop = (r.effective == 0 && r.overheal > 0);
    if (r.effective > 0) {
        const auto mv = roles_.ModifyHp(cid, r.effective, req.trace);
        if (!mv.HasValue()) return core::Result<HealResult>::Fail(mv.Err());
    }
    r.remaining_hp = tc->hp;

    // overheal_allowed=false（默认）：满血治疗不发布事件、不计入有效治疗量，
    // 防止对满血目标刷治疗量污染 HPS 统计（§6 观测性）。
    const bool counted = !full_hp_noop || req.overheal_allowed;
    if (counted) {
        ++stats_.heal_events;
        if (r.is_crit) ++stats_.heal_crit_count;
        stats_.total_heal += static_cast<std::uint64_t>(r.effective);
        if (formula_.heal_overheal_counts_to_stats) {
            stats_.total_overheal += static_cast<std::uint64_t>(r.overheal);
        }
        if (events_ != nullptr) {
            HealEvent ev{};
            ev.caster = req.source;
            ev.target = req.target;
            ev.amount = r.effective;
            ev.trace = req.trace;
            (void)events_->Publish(ev);
        }
    }

    (void)ctx;  // 治疗侧不参与采样日志（采样只针对伤害，§15.8）
    return core::Result<HealResult>::Ok(r);
}

}  // namespace mmo::game::combat
