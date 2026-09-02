// server/gamenode/world/src/instance_manager.cpp — TASK-020 §7 / §8 / §15.2~§15.5 / §19
//
// 副本实例生命周期：五状态机 + Create/Start/Complete/Destroy/AddMember/RemoveMember
// + Tick（超时/空实例/全员退出回收 + 单 Tick 分批析构，上限 10）。
// 加载失败 / Scene 创建失败均不产生悬挂实例（§19 Forbidden）。

#include "mmo/game/world/instance_manager.h"

#include <algorithm>

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/scene/scene.h"
#include "mmo/game/scene/scene_context.h"

namespace mmo::game::world {

using namespace mmo::core;

InstanceManager::InstanceManager(SceneManager& scenes,
                                 ai::AiSystem& ai,
                                 EntityManager& entities,
                                 core::EventBus& events,
                                 core::Scheduler& scheduler,
                                 core::Arena& arena,
                                 NodeId owner_node) noexcept
    : scenes_(scenes), ai_(ai), entities_(entities), events_(events),
      scheduler_(scheduler), arena_(arena), owner_node_(owner_node),
      now_(core::MonotonicClock::Point()) {}

void InstanceManager::SetReclaimPolicy(core::DurationMs empty_instance_timeout,
                                       core::DurationMs destroy_grace) noexcept {
    empty_timeout_ = empty_instance_timeout;
    destroy_grace_ = destroy_grace;
}

core::Result<void> InstanceManager::LoadConfig(std::string_view dir) {
    auto b = LoadWorldConfig(dir);
    if (!b) return core::Result<void>::Fail(b.Err());
    bundle_ = std::move(b).Value();
    return core::Result<void>::Ok();
}

const InstanceDef* InstanceManager::LookupDef(std::uint32_t def_id) const {
    auto it = bundle_.instances.find(def_id);
    return it == bundle_.instances.end() ? nullptr : &it->second;
}

Instance* InstanceManager::Mutable(InstanceId id) noexcept {
    auto it = instances_.find(id);
    return it == instances_.end() ? nullptr : &it->second;
}

SceneType InstanceManager::SceneTypeFor(InstanceType t) const noexcept {
    return static_cast<SceneType>(static_cast<std::uint8_t>(t));
}

core::Result<InstanceId> InstanceManager::Create(
    std::uint32_t def_id,
    std::span<const mmo::game::PlayerId> members,
    core::TraceID /*trace*/) {
    const InstanceDef* def = LookupDef(def_id);
    if (def == nullptr) {
        return core::Result<InstanceId>::Fail(
            Error(ErrorCode::NOT_FOUND, "unknown instance def_id", domain::kCore));
    }
    const InstanceId id = ++next_id_;
    const SceneId sid = mmo::game::MakeSceneId(
        static_cast<std::uint8_t>(SceneTypeFor(def->type)),
        ++next_scene_index_);
    Instance inst;
    inst.id = id;
    inst.def_id = def_id;
    inst.scene_id = sid;
    inst.state = InstanceState::Pending;
    inst.created_at = Now();
    inst.started_at = core::SteadyTime{};
    inst.elapsed = core::DurationMs{0};
    inst.version = 0;
    inst.members.assign(members.begin(), members.end());
    inst.ever_entered = !members.empty();
    inst.destroy_at = core::SteadyTime{};
    inst.pending_reclaim = false;
    inst.last_result = InstanceResult::Cleared;

    instances_.emplace(id, std::move(inst));
    ++create_count_;
    return core::Result<InstanceId>::Ok(id);
}

core::Result<void> InstanceManager::Start(InstanceId id, core::TraceID /*trace*/) {
    Instance* inst = Mutable(id);
    if (inst == nullptr) {
        return core::Result<void>::Fail(
            Error(ErrorCode::NOT_FOUND, "instance not found", domain::kCore));
    }
    if (inst->state != InstanceState::Pending) {
        return core::Result<void>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "instance not pending", domain::kCore));
    }
    // Pending -> Loading
    inst->state = InstanceState::Loading;
    const auto* def = LookupDef(inst->def_id);
    if (def == nullptr) {
        inst->state = InstanceState::Destroying;
        inst->pending_reclaim = true;
        inst->destroy_at = Now();
        return core::Result<void>::Fail(
            Error(ErrorCode::NOT_FOUND, "instance def missing", domain::kCore));
    }
    const SceneType st = SceneTypeFor(def->type);

    // 创建 Scene；失败转 Destroying，不留下悬挂 Scene（§19）。
    auto sres = scenes_.Create(inst->scene_id, st, owner_node_);
    if (!sres) {
        inst->state = InstanceState::Destroying;
        inst->pending_reclaim = true;
        inst->destroy_at = Now();
        return core::Result<void>::Fail(sres.Err());
    }

    // Loading -> 生成怪物/NPC（best-effort：单只失败不阻断副本启动）。
    mmo::game::SceneContext ctx(inst->scene_id, st, owner_node_, Now(), 0,
                                entities_, events_, scheduler_, arena_);
    for (const auto& sd : def->spawn_defs) {
        auto sp = ai_.Spawn(sd, ctx);
        if (sp) spawned_[id].push_back(sp.Value());
    }

    // Loading -> Running
    inst->state = InstanceState::Running;
    inst->started_at = Now();
    return core::Result<void>::Ok();
}

core::Result<void> InstanceManager::Complete(InstanceId id, InstanceResult result,
                                              core::TraceID /*trace*/) {
    Instance* inst = Mutable(id);
    if (inst == nullptr) {
        return core::Result<void>::Fail(
            Error(ErrorCode::NOT_FOUND, "instance not found", domain::kCore));
    }
    if (inst->state != InstanceState::Running) {
        return core::Result<void>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "instance not running", domain::kCore));
    }
    inst->state = InstanceState::Completed;
    inst->last_result = result;
    inst->pending_reclaim = true;
    inst->destroy_at = Now() + destroy_grace_;
    return core::Result<void>::Ok();
}

core::Result<void> InstanceManager::Destroy(InstanceId id, core::TraceID /*trace*/) {
    auto it = instances_.find(id);
    if (it == instances_.end()) {
        return core::Result<void>::Fail(
            Error(ErrorCode::NOT_FOUND, "instance not found", domain::kCore));
    }
    // 回收怪物/NPC 实体（防泄漏，§17 集成）。
    auto sit = spawned_.find(id);
    if (sit != spawned_.end()) {
        for (mmo::game::EntityId e : sit->second) (void)ai_.Despawn(e);
        spawned_.erase(sit);
    }
    (void)scenes_.Destroy(it->second.scene_id);
    instances_.erase(it);
    ++destroy_count_;
    return core::Result<void>::Ok();
}

core::Result<void> InstanceManager::AddMember(InstanceId id, mmo::game::PlayerId player,
                                               core::TraceID /*trace*/) {
    Instance* inst = Mutable(id);
    if (inst == nullptr) {
        return core::Result<void>::Fail(
            Error(ErrorCode::NOT_FOUND, "instance not found", domain::kCore));
    }
    if (inst->state != InstanceState::Loading && inst->state != InstanceState::Running) {
        return core::Result<void>::Fail(
            Error(ErrorCode::INVALID_ARGUMENT, "instance not joinable", domain::kCore));
    }
    for (auto p : inst->members) {
        if (p == player) {
            return core::Result<void>::Fail(
                Error(ErrorCode::INVALID_ARGUMENT, "already member", domain::kCore));
        }
    }
    const auto* def = LookupDef(inst->def_id);
    if (inst->members.size() >= def->max_players) {
        return core::Result<void>::Fail(
            Error(ErrorCode::BUSY, "instance full", domain::kCore));
    }
    inst->members.push_back(player);
    inst->ever_entered = true;
    return core::Result<void>::Ok();
}

core::Result<void> InstanceManager::RemoveMember(InstanceId id, mmo::game::PlayerId player,
                                                  LeaveReason /*reason*/, core::TraceID /*trace*/) {
    Instance* inst = Mutable(id);
    if (inst == nullptr) {
        return core::Result<void>::Fail(
            Error(ErrorCode::NOT_FOUND, "instance not found", domain::kCore));
    }
    auto& m = inst->members;
    auto it = std::find(m.begin(), m.end(), player);
    if (it == m.end()) {
        return core::Result<void>::Fail(
            Error(ErrorCode::NOT_FOUND, "not a member", domain::kCore));
    }
    m.erase(it);
    if (m.empty()
        && (inst->state == InstanceState::Loading
            || inst->state == InstanceState::Running
            || inst->state == InstanceState::Completed)) {
        inst->state = InstanceState::Destroying;
        inst->pending_reclaim = true;
        inst->destroy_at = Now() + destroy_grace_;
    }
    return core::Result<void>::Ok();
}

core::Result<void> InstanceManager::Tick(core::SteadyTime now) {
    now_ = now;
    const std::int64_t empty_to_ns = static_cast<std::int64_t>(empty_timeout_.count()) * 1'000'000;
    std::vector<InstanceId> to_reclaim;

    for (auto& kv : instances_) {
        Instance& inst = kv.second;
        const auto* def = LookupDef(inst.def_id);
        switch (inst.state) {
        case InstanceState::Running: {
            // 超时 -> Completed
            if (def != nullptr && def->time_limit.count() > 0
                && core::MonotonicClock::Elapsed(inst.started_at, now)
                       >= static_cast<std::int64_t>(def->time_limit.count()) * 1'000'000) {
                inst.state = InstanceState::Completed;
                inst.pending_reclaim = true;
                inst.destroy_at = now + destroy_grace_;
            } else if (!inst.ever_entered
                       && core::MonotonicClock::Elapsed(inst.created_at, now) >= empty_to_ns) {
                // 空实例（从未进入）回收
                inst.state = InstanceState::Destroying;
                inst.pending_reclaim = true;
                inst.destroy_at = now;
            } else if (inst.members.empty()) {
                // 全员退出：延迟宽限（防误杀，§8）
                inst.state = InstanceState::Destroying;
                inst.pending_reclaim = true;
                inst.destroy_at = now + destroy_grace_;
            }
            break;
        }
        case InstanceState::Completed:
            if (inst.pending_reclaim && now >= inst.destroy_at) {
                inst.state = InstanceState::Destroying;
                to_reclaim.push_back(inst.id);  // 同趟回收，无需多一次 Tick
            }
            break;
        case InstanceState::Destroying:
            if (now >= inst.destroy_at) to_reclaim.push_back(inst.id);
            break;
        case InstanceState::Pending:
        case InstanceState::Loading:
            if (!inst.ever_entered
                && core::MonotonicClock::Elapsed(inst.created_at, now) >= empty_to_ns) {
                inst.state = InstanceState::Destroying;
                inst.pending_reclaim = true;
                inst.destroy_at = now;
            }
            break;
        }
    }

    // 分批回收，单 Tick 上限 10（防尖峰，§19 / §21 Forbidden）
    std::size_t n = 0;
    for (InstanceId id : to_reclaim) {
        if (n >= kMaxReclaimPerTick) break;
        auto it = instances_.find(id);
        if (it != instances_.end()) {
            // 回收怪物/NPC 实体（防泄漏，§17 集成）。
            auto sit = spawned_.find(id);
            if (sit != spawned_.end()) {
                for (mmo::game::EntityId e : sit->second) (void)ai_.Despawn(e);
                spawned_.erase(sit);
            }
            (void)scenes_.Destroy(it->second.scene_id);
            instances_.erase(it);
            ++destroy_count_;
            ++n;
        }
    }
    return core::Result<void>::Ok();
}

const Instance* InstanceManager::Find(InstanceId id) const noexcept {
    auto it = instances_.find(id);
    return it == instances_.end() ? nullptr : &it->second;
}

std::size_t InstanceManager::CountByState(InstanceState s) const noexcept {
    std::size_t c = 0;
    for (const auto& kv : instances_) {
        if (kv.second.state == s) ++c;
    }
    return c;
}

}  // namespace mmo::game::world
