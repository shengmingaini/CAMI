// server/gamenode/ai/src/ai_system.cpp — TASK-018 §7 / §8 / §15
//
// 实现：Spawn/Despawn/Update/OnDamaged + 表驱动状态机 + 决策节流(5Hz) + AOI 局部目标选取
//       + Movement 追击/返回意图 + Scheduler 死亡重生（不建线程、不每怪一个定时器）。
//
// 红线（§21）：禁止每 Tick 全量决策（节流）、禁止全 Scene 扫描选目标（走 AOI）、
//              禁止为每个怪创建线程、禁止硬编码 NPC 数值（全部来自 SpawnDef）。

#include "mmo/game/ai/ai_system.h"

#include <cmath>
#include <vector>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/time/clock.h"

namespace mmo::game::ai {

using namespace mmo::core;  // Result / Error / ErrorCode / SteadyTime / DurationMs / MonotonicClock

// ---- AI 行为常量（非 NPC 属性，属移动行为调参，可在 combat/配置里再细化） ----
constexpr float kChaseSpeed  = 5.0f;   // 追击速度 m/s
constexpr float kPatrolSpeed = 2.0f;   // 巡逻 / 返回速度 m/s
constexpr float kAttackRange = 2.5f;   // 进入攻击距离 m
constexpr float kArriveEps   = 1.0f;   // 到达路点/原点判定阈值 m
constexpr std::uint32_t kWaypoints = 8;
constexpr double kPi = 3.14159265358979323846;

namespace {

inline movement::Vec3 ToVec(const mmo::game::Position& p) noexcept {
    return movement::Vec3{p.x, p.y, p.z};
}
inline movement::Vec3 Norm(movement::Vec3 v) noexcept {
    const float len = movement::Vec3Length(v);
    if (len <= 1e-6f) return movement::Vec3{};
    return v * (1.0f / len);
}

}  // namespace

AiSystem::AiSystem(mmo::game::EntityManager& entities,
                   mmo::game::movement::MovementSystem& movement,
                   mmo::game::aoi::IAoi& aoi,
                   mmo::core::Scheduler& scheduler) noexcept
    : entities_(entities), movement_(movement), aoi_(aoi), scheduler_(scheduler) {
    visible_scratch_.reserve(64);
}

core::Result<mmo::game::EntityId> AiSystem::Spawn(const SpawnDef& def,
                                               const mmo::game::SceneContext& ctx) {
    auto e = entities_.Create(def.type, ctx.id, def.spawn_pos);
    if (!e) return core::Result<mmo::game::EntityId>::Fail(e.Err());

    const mmo::game::EntityId id = e.Value()->Id();
    AiComponent comp;
    comp.state = AiState::Idle;
    comp.target = kInvalidEntity;
    comp.spawn_origin = def.spawn_pos;
    comp.state_entered_at = ctx.now;
    comp.next_decision_at = ctx.now;  // 首帧即可决策
    comp.patrol_index = 0;
    comps_.emplace(id, comp);

    hp_.emplace(id, def.max_hp);
    runtime_.emplace(id, Runtime{def.respawn_seconds, def.max_hp, def.aggro_radius,
                                  def.chase_leave_radius, def.patrol_radius, def.level});
    state_counts_[Idx(AiState::Idle)]++;

    // 登记移动 + AOI（重生/目标选取/追击都依赖这两者）。
    movement::MovementState ms;
    ms.pos = def.spawn_pos;
    ms.max_speed = kChaseSpeed;
    (void)movement_.Register(id, ms);
    (void)aoi_.Enter(id, def.spawn_pos);

    return core::Result<mmo::game::EntityId>::Ok(id);
}

core::Result<void> AiSystem::Despawn(mmo::game::EntityId id) {
    auto it = comps_.find(id);
    if (it == comps_.end()) {
        return core::Result<void>::Fail(
            Error(ErrorCode::NOT_FOUND, "ai entity not found", domain::kCore));
    }
    if (it->second.state != AiState::Dead) {
        state_counts_[Idx(it->second.state)]--;
    } else {
        state_counts_[Idx(AiState::Dead)]--;
    }
    // 取消 pending 重生定时器（不建线程，定时器由 Scheduler 统一管理）。
    auto rit = respawn_timers_.find(id);
    if (rit != respawn_timers_.end()) {
        (void)scheduler_.Cancel(rit->second);
        respawn_timers_.erase(rit);
    }
    comps_.erase(it);
    hp_.erase(id);
    runtime_.erase(id);
    (void)movement_.Stop(id);
    (void)aoi_.Leave(id);
    return entities_.Destroy(id);
}

core::Result<void> AiSystem::Update(const mmo::game::SceneContext& ctx) {
    const core::SteadyTime now = ctx.now;
    for (auto& kv : comps_) {
        AiComponent& comp = kv.second;
        if (comp.state == AiState::Dead) continue;  // 死亡态由 Scheduler 重生，不决策
        if (now < comp.next_decision_at) continue;   // 节流：未到决策时刻，仅维持既有移动意图
        ++decision_count_;
        DecideAndAct(kv.first, comp, ctx);
        comp.next_decision_at = now + decision_interval_;
    }
    return core::Result<void>::Ok();
}

mmo::game::EntityId AiSystem::PickTarget(mmo::game::EntityId id, const mmo::game::Position& my_pos,
                                      float aggro) const {
    visible_scratch_.clear();
    if (!aoi_.QueryVisible(id, visible_scratch_).HasValue()) return kInvalidEntity;
    mmo::game::EntityId best = kInvalidEntity;
    float best_d = aggro + 1.0f;  // 必须落在仇恨半径内
    for (mmo::game::EntityId t : visible_scratch_) {
        if (t == id) continue;
        const auto* e = entities_.Find(t);
        if (e == nullptr) continue;
        if (e->Type() != mmo::game::EntityType::Player) continue;  // 敌对目标 = 玩家
        const float d = movement::Distance3(my_pos, e->Pos());
        if (d <= aggro && d < best_d) {
            best_d = d;
            best = t;
        }
    }
    return best;
}

void AiSystem::DecideAndAct(mmo::game::EntityId id, AiComponent& comp,
                            const mmo::game::SceneContext& ctx) {
    auto* me = entities_.Find(id);
    if (me == nullptr) return;
    const mmo::game::Position my_pos = me->Pos();
    const auto rt = runtime_.find(id);
    const float aggro = rt != runtime_.end() ? rt->second.aggro_radius : 15.0f;
    const float chase_leave = rt != runtime_.end() ? rt->second.chase_leave_radius : 30.0f;
    const float patrol_r = rt != runtime_.end() ? rt->second.patrol_radius : 10.0f;

    // 目标获取：已有有效目标（追击/攻击态）则复用，跳过一次 AOI 网格扫描（省成本）；
    // 仅当目标失效（销毁）或超出追击半径时才重新扫描。无目标（Idle/Patrol）才每决策周期扫一次。
    mmo::game::EntityId target = comp.target;
    if (target != kInvalidEntity) {
        const auto* te = entities_.Find(target);
        const float td = te != nullptr ? movement::Distance3(my_pos, te->Pos()) : 1e9f;
        if (te == nullptr || td > chase_leave) {
            target = PickTarget(id, my_pos, aggro);  // 失效/超距 -> 重选
        }
        // 否则保留既有目标，不重扫网格
    } else {
        target = PickTarget(id, my_pos, aggro);
    }

    AiState next = comp.state;
    if (target != kInvalidEntity) {
        const auto* te = entities_.Find(target);
        const float d = te != nullptr ? movement::Distance3(my_pos, te->Pos()) : 1e9f;
        if (d <= kAttackRange)            next = AiState::Attack;
        else if (d > chase_leave)         next = AiState::Return;  // 脱离追击半径
        else                              next = AiState::Chase;
    } else {
        switch (comp.state) {
            case AiState::Attack:
            case AiState::Chase:
                next = AiState::Return;  // 目标消失 -> 返程
                break;
            case AiState::Return:
                next = (movement::Distance3(my_pos, comp.spawn_origin) <= kArriveEps)
                            ? AiState::Idle
                            : AiState::Return;
                break;
            case AiState::Idle:
            case AiState::Patrol:
            default:
                next = AiState::Patrol;  // 无目标继续巡逻
                break;
        }
    }

    // 按下一态下发移动意图（Movement 在 Integrate 阶段积分；AI 不写权威坐标）。
    movement::Vec3 vel{};
    switch (next) {
        case AiState::Patrol: {
            const double ang = kPi * 2.0 * static_cast<double>(comp.patrol_index % kWaypoints)
                               / static_cast<double>(kWaypoints);
            mmo::game::Position wp = comp.spawn_origin;
            wp.x += patrol_r * static_cast<float>(std::cos(ang));
            wp.z += patrol_r * static_cast<float>(std::sin(ang));
            movement::Vec3 dir = ToVec(wp) - ToVec(my_pos);
            if (movement::Vec3LengthSq(dir) <= kArriveEps * kArriveEps) {
                comp.patrol_index = (comp.patrol_index + 1) % kWaypoints;  // 到点换下一站
            } else {
                vel = Norm(dir) * kPatrolSpeed;
            }
            break;
        }
        case AiState::Chase: {
            const auto* te = entities_.Find(target);
            if (te != nullptr) vel = Norm(ToVec(te->Pos()) - ToVec(my_pos)) * kChaseSpeed;
            break;
        }
        case AiState::Return: {
            vel = Norm(ToVec(comp.spawn_origin) - ToVec(my_pos)) * kPatrolSpeed;
            break;
        }
        case AiState::Attack:
        case AiState::Idle:
        case AiState::Dead:
        default:
            vel = {};  // 停下
            break;
    }
    (void)movement_.SetVelocity(id, vel);

    // 表驱动提交转移（未授权则保持原态，绝不静默跳变）。
    if (next != comp.state && CanTransition(comp.state, next)) {
        state_counts_[Idx(comp.state)]--;
        comp.state = next;
        state_counts_[Idx(next)]++;
        comp.state_entered_at = ctx.now;
    }
    comp.target = target;
}

core::Result<void> AiSystem::OnDamaged(mmo::game::EntityId victim, mmo::game::EntityId attacker,
                                       std::int64_t amount) {
    auto it = comps_.find(victim);
    if (it == comps_.end()) return core::Result<void>::Ok();  // 非 AI 实体
    AiComponent& comp = it->second;
    if (comp.state == AiState::Dead) return core::Result<void>::Ok();

    std::int64_t& hp = hp_[victim];
    hp -= amount;

    // 拉仇恨：被攻击即把攻击者设为目标，并从被动态拉入 Chase（表驱动校验）。
    if (attacker != kInvalidEntity) {
        comp.target = attacker;
        if (comp.state == AiState::Idle || comp.state == AiState::Patrol
            || comp.state == AiState::Return) {
            if (CanTransition(comp.state, AiState::Chase)) {
                state_counts_[Idx(comp.state)]--;
                comp.state = AiState::Chase;
                state_counts_[Idx(AiState::Chase)]++;
                comp.state_entered_at = MonotonicClock::Point();
            }
        }
    }

    if (hp <= 0) {
        if (CanTransition(comp.state, AiState::Dead)) {
            state_counts_[Idx(comp.state)]--;
            comp.state = AiState::Dead;
            state_counts_[Idx(AiState::Dead)]++;
        }
        comp.target = kInvalidEntity;
        (void)movement_.Stop(victim);
        const std::uint32_t resp = runtime_.count(victim) ? runtime_[victim].respawn_seconds : 30u;
        auto tid = scheduler_.ScheduleAfter(DurationMs(resp * 1000),
            [this, victim]() noexcept { Revive(victim); });  // 32B 内联可调用，不建线程
        if (tid.HasValue()) respawn_timers_[victim] = tid.Value();
    }
    return core::Result<void>::Ok();
}

void AiSystem::Revive(mmo::game::EntityId id) noexcept {
    auto it = comps_.find(id);
    if (it == comps_.end()) return;
    auto* e = entities_.Find(id);
    if (e == nullptr) {  // 实体已被销毁，清理残留
        comps_.erase(it);
        hp_.erase(id);
        runtime_.erase(id);
        respawn_timers_.erase(id);
        return;
    }
    const auto rt = runtime_.find(id);
    const std::int64_t max_hp = rt != runtime_.end() ? rt->second.max_hp : 100;

    hp_[id] = max_hp;
    AiComponent& comp = it->second;
    if (comp.state == AiState::Dead) state_counts_[Idx(AiState::Dead)]--;
    comp.state = AiState::Idle;
    comp.target = kInvalidEntity;
    comp.patrol_index = 0;
    comp.state_entered_at = MonotonicClock::Point();
    comp.next_decision_at = comp.state_entered_at;
    state_counts_[Idx(AiState::Idle)]++;

    // 传送回重生点（权威坐标 + 移动状态 + AOI 同步）。
    e->SetPos(comp.spawn_origin);
    auto st = movement_.StateOf(id);
    if (st.HasValue() && st.Value() != nullptr) {
        st.Value()->pos = comp.spawn_origin;
        st.Value()->velocity = {};
    }
    (void)aoi_.Move(id, comp.spawn_origin);
    respawn_timers_.erase(id);
}

AiState AiSystem::StateOf(mmo::game::EntityId id) const noexcept {
    auto it = comps_.find(id);
    return it != comps_.end() ? it->second.state : AiState::Idle;
}

std::size_t AiSystem::CountByState(AiState s) const noexcept {
    return state_counts_[Idx(s)];
}

mmo::game::EntityId AiSystem::TargetOf(mmo::game::EntityId id) const noexcept {
    auto it = comps_.find(id);
    return it != comps_.end() ? it->second.target : kInvalidEntity;
}

}  // namespace mmo::game::ai
