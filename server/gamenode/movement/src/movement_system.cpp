/// TASK-015 · Movement System 实现（§7 / §8 / §15）。
///
/// 热路径：禁止 DB / Redis / gRPC / 文件 IO / 网络阻塞 IO / 大规模分配（§10 / §11 / §21）。
/// 所有拒绝必须计数（rejected_by_reason），便于反作弊运营（§6 / §8）。

#include "mmo/game/movement/movement_system.h"

#include <chrono>
#include <cmath>

#include "mmo/core/error/error.h"                 // Error
#include "mmo/core/error/error_code.h"            // ErrorCode / domain::kCore
#include "mmo/core/time/clock.h"                 // core::SteadyTime
#include "mmo/game/entity/entity_manager.h"       // Entity / EntityManager
#include "mmo/game/movement/validator.h"          // ValidateMovement

namespace mmo::game::movement {

// ---------------------------------------------------------------------------
// 状态访问
// ---------------------------------------------------------------------------

MovementState& MovementSystem::EnsureState(EntityId id) {
    auto it = states_.find(id);
    if (it == states_.end()) {
        MovementState s;
        s.max_speed = cfg_.max_speed;
        it = states_.emplace(id, s).first;
    }
    return it->second;
}

core::Result<void> MovementSystem::Register(EntityId id, const MovementState& init) {
    states_[id] = init;
    return core::Result<void>::Ok();
}

core::Result<MovementState*> MovementSystem::StateOf(EntityId id) {
    auto it = states_.find(id);
    if (it == states_.end()) {
        return core::Result<MovementState*>::Fail(
            core::Error(core::ErrorCode::NOT_FOUND, "movement state not found", core::domain::kCore));
    }
    return core::Result<MovementState*>::Ok(&it->second);
}

core::Result<MoveReject> MovementSystem::Validate(const MoveCommand& cmd,
                                                  const MovementState& state) const noexcept {
    return core::Result<MoveReject>::Ok(ValidateMovement(cmd, state, cfg_));
}

core::Result<void> MovementSystem::SetSpeed(EntityId id, float max_speed) {
    if (!std::isfinite(max_speed) || max_speed < 0.0f) {
        return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
                                                    "bad speed", core::domain::kCore));
    }
    auto it = states_.find(id);
    if (it == states_.end()) {
        return core::Result<void>::Fail(core::Error(core::ErrorCode::NOT_FOUND,
                                                     "movement state not found", core::domain::kCore));
    }
    it->second.max_speed = max_speed;
    return core::Result<void>::Ok();
}

core::Result<void> MovementSystem::SetVelocity(EntityId id, Vec3 v) {
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
        return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
                                                    "non-finite velocity", core::domain::kCore));
    }
    auto it = states_.find(id);
    if (it == states_.end()) {
        return core::Result<void>::Fail(core::Error(core::ErrorCode::NOT_FOUND,
                                                     "movement state not found", core::domain::kCore));
    }
    it->second.velocity = v;
    it->second.speed = Vec3Length(v);
    return core::Result<void>::Ok();
}

core::Result<void> MovementSystem::Stop(EntityId id) {
    auto it = states_.find(id);
    if (it == states_.end()) {
        return core::Result<void>::Fail(core::Error(core::ErrorCode::NOT_FOUND,
                                                     "movement state not found", core::domain::kCore));
    }
    it->second.velocity = Vec3{0.0f, 0.0f, 0.0f};
    it->second.speed = 0.0f;
    return core::Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// 命令应用（§15.3）
// ---------------------------------------------------------------------------

core::Result<void> MovementSystem::ApplyCommand(const MoveCommand& cmd,
                                                const SceneContext& ctx) {
    stats_.total_commands++;

    // 非有限坐标（NaN/Inf）→ 拒绝并保持原位置（§19）。
    if (!IsFinitePos(cmd.to) || !IsFinitePos(cmd.from)) {
        stats_.CountReject(MoveReject::OutOfBounds);
        return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
                                                     "non-finite coordinate", core::domain::kCore));
    }

    // 实体已销毁 / 未知 → NOT_FOUND，不崩溃（§19）。
    Entity* e = ctx.entities.Find(cmd.entity);
    if (e == nullptr) {
        return core::Result<void>::Fail(core::Error(core::ErrorCode::NOT_FOUND,
                                                     "entity unknown or destroyed", core::domain::kCore));
    }

    MovementState& st = EnsureState(cmd.entity);
    const MoveReject r = ValidateMovement(cmd, st, cfg_);

    Position target;
    switch (r) {
        case MoveReject::None:
            target = cmd.to;
            break;
        case MoveReject::TooFast:        // 钳制到合法位置后接受（§8）
            target = ClampSpeed(st.pos, cmd.to, cfg_);
            stats_.CountReject(MoveReject::TooFast);
            break;
        case MoveReject::OutOfBounds:
            // 非有限坐标已在入口拒绝；此处区分两类越界（§8 / §19）：
            //   · 极端大坐标（1e30 等）→ 不可钳制，直接拒绝，防浮点溢出传播；
            //   · 有限越界（贴边小幅移动）→ 钳制到边界后接受。
            stats_.CountReject(MoveReject::OutOfBounds);
            if (IsExtremePos(cmd.to, cfg_.world_half)) {
                return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
                                                            "extreme coordinate", core::domain::kCore));
            }
            target = ClampBounds(cmd.to, cfg_);
            break;
        case MoveReject::Teleport:       // 拒绝 + 回拉（§8）
            stats_.CountReject(MoveReject::Teleport);
            return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
                                                        "teleport", core::domain::kCore));
        case MoveReject::NotMovable:     // 拒绝（§8）
            stats_.CountReject(MoveReject::NotMovable);
            return core::Result<void>::Fail(core::Error(core::ErrorCode::INVALID_ARGUMENT,
                                                        "not movable", core::domain::kCore));
        case MoveReject::RateLimited:    // 拒绝（§8）
            stats_.CountReject(MoveReject::RateLimited);
            return core::Result<void>::Fail(core::Error(core::ErrorCode::RATE_LIMITED,
                                                        "rate limited", core::domain::kCore));
    }

    // 纠偏判定（§15.5）：权威 target 与客户端意图 to 偏差 / 客户端 from 与权威 pos 偏差。
    if (Distance3(target, cmd.to) > cfg_.correction_threshold) stats_.corrections++;
    if (Distance3(cmd.from, st.pos) > cfg_.correction_threshold) stats_.corrections++;

    // 计算权威速度（位移 / dt），写回状态。
    const Vec3 delta{target.x - st.pos.x, target.y - st.pos.y, target.z - st.pos.z};
    const float inv_dt = (cfg_.tick_dt > 0.0f) ? 1.0f / cfg_.tick_dt : 0.0f;
    st.velocity = delta * inv_dt;
    st.speed = Vec3Length(st.velocity);
    st.pos = target;
    st.last_client_seq = cmd.client_seq;
    st.last_client_timestamp_ms = cmd.client_timestamp_ms;
    st.last_client_update = ctx.now;
    st.moved_tick = ctx.tick_number;  // 本 Tick 由命令驱动，Integrate 不再重复推进

    // 写回实体权威坐标。
    e->SetPos(target);
    stats_.moved_count++;

    // 事件 + AOI 联动（§15.6）。
    CommitPosition(cmd.entity, target, ctx);
    return core::Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// Tick 积分（§15.4）
// ---------------------------------------------------------------------------

core::Result<void> MovementSystem::Integrate(const SceneContext& ctx, float dt_seconds) {
    const float dt = (dt_seconds > 0.0f) ? dt_seconds : cfg_.tick_dt;
    for (auto& kv : states_) {
        const EntityId id = kv.first;
        MovementState& st = kv.second;

        // 本 Tick 已由命令驱动移动过 → 不重复推进（避免双计位移）。
        if (st.moved_tick == ctx.tick_number) continue;

        if (Vec3LengthSq(st.velocity) <= 0.0f) continue;  // 无速度跳过

        // 外推超时：超过 stop_timeout 无命令 → 停止（§19）。
        const float since_sec =
            std::chrono::duration<float>(ctx.now - st.last_client_update).count();
        if (since_sec > cfg_.stop_timeout_sec) {
            st.velocity = Vec3{0.0f, 0.0f, 0.0f};
            st.speed = 0.0f;
            continue;
        }

        Vec3 delta = st.velocity * dt;
        Position np;
        np.x = st.pos.x + delta.x;
        np.y = st.pos.y + delta.y;
        np.z = st.pos.z + delta.z;
        np.yaw = st.pos.yaw;
        np = ClampBounds(np, cfg_);  // 地形/世界边界钳制（§8）

        st.pos = np;
        if (Entity* e = ctx.entities.Find(id)) e->SetPos(np);
        stats_.moved_count++;

        CommitPosition(id, np, ctx);
    }
    return core::Result<void>::Ok();
}

// ---------------------------------------------------------------------------
// 私有
// ---------------------------------------------------------------------------

void MovementSystem::CommitPosition(EntityId id, const Position& pos,
                                     const SceneContext& ctx) {
    // 发布 EntityMoved（24B 值类型，EventBus 内联预算）。
    (void)ctx.events.Publish(EntityMoved{id, pos});

    // AOI 联动：位置变化后调用 IAoi::Move，并把 entered/left 转 EventBus 事件（§15.6）。
    if (aoi_ != nullptr) {
        auto mv = aoi_->Move(id, pos);
        if (!mv) {
            stats_.aoi_errors++;  // 失败记录但不回滚位置（§19）
            return;
        }
        const aoi::MoveResult& res = mv.Value();
        for (EntityId w : res.entered) (void)ctx.events.Publish(EntityViewEnter{w, id});
        for (EntityId w : res.left)    (void)ctx.events.Publish(EntityViewLeave{w, id});
    }
}

Position MovementSystem::ClampSpeed(const Position& origin, const Position& to,
                                    const MovementConfig& cfg) noexcept {
    const Vec3 d{to.x - origin.x, to.y - origin.y, to.z - origin.z};
    const float len = Vec3Length(d);
    const float max_step = cfg.max_speed * cfg.tick_dt * cfg.speed_tolerance;
    if (len <= max_step || len <= 0.0f) return to;
    const float scale = max_step / len;
    Position p;
    p.x = origin.x + d.x * scale;
    p.y = origin.y + d.y * scale;
    p.z = origin.z + d.z * scale;
    p.yaw = to.yaw;
    return p;
}

Position MovementSystem::ClampBounds(const Position& to, const MovementConfig& cfg) noexcept {
    Position p = to;
    const float w = cfg.world_half;
    if (p.x < -w) p.x = -w; else if (p.x > w) p.x = w;
    if (p.y < -w) p.y = -w; else if (p.y > w) p.y = w;
    if (p.z < -w) p.z = -w; else if (p.z > w) p.z = w;
    return p;
}

}  // namespace mmo::game::movement
