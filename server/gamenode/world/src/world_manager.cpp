// server/gamenode/world/src/world_manager.cpp — TASK-020 §7 / §15.6~§15.8 / §19
//
// OpenWorld 常驻场景 + 分线（300 人阈值）+ TransferPlayer（Scene Enter/Leave，禁搬实体）
// + 全局 Tick 驱动（Scene + 实例回收）+ 指标。

#include "mmo/game/world/world_manager.h"

#include "mmo/core/error/error.h"
#include "mmo/core/error/error_code.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/scene/scene.h"

namespace mmo::game::world {

using namespace mmo::core;

WorldManager::WorldManager(SceneManager& scenes,
                           InstanceManager& instances,
                           EntityManager& entities,
                           NodeId owner_node) noexcept
    : scenes_(scenes), instances_(instances), entities_(entities), owner_node_(owner_node),
      now_(core::MonotonicClock::Point()) {}

core::Result<void> WorldManager::Init(const WorldConfig& cfg) {
    cfg_ = cfg;
    // 回收策略透传到实例管理器（空实例超时 / 全员退出宽限）。
    instances_.SetReclaimPolicy(cfg.empty_instance_timeout, cfg.destroy_grace);
    return core::Result<void>::Ok();
}

core::Result<void> WorldManager::LoadConfig(std::string_view dir) {
    auto b = LoadWorldConfig(dir);
    if (!b) return core::Result<void>::Fail(b.Err());
    world_defs_ = std::move(b.Value().worlds);
    return core::Result<void>::Ok();
}

core::Result<mmo::game::SceneId> WorldManager::GetOrCreateOpenWorld(std::uint32_t world_def_id) {
    if (world_defs_.find(world_def_id) == world_defs_.end()) {
        return core::Result<mmo::game::SceneId>::Fail(
            Error(ErrorCode::NOT_FOUND, "unknown world def_id", domain::kCore));
    }
    auto& shards = open_worlds_[world_def_id];
    // 返回未满分线
    for (mmo::game::SceneId sid : shards) {
        auto sres = scenes_.Find(sid);
        if (sres && sres.Value()->PlayerCount() < cfg_.sharding_threshold) {
            return core::Result<mmo::game::SceneId>::Ok(sid);
        }
    }
    // 全满或尚无：新建分线
    const mmo::game::SceneId new_sid = mmo::game::MakeSceneId(
        static_cast<std::uint8_t>(SceneType::World), ++world_scene_index_);
    auto cres = scenes_.Create(new_sid, SceneType::World, owner_node_);
    if (!cres) {
        return core::Result<mmo::game::SceneId>::Fail(cres.Err());
    }
    // OpenWorld 分线由 WorldManager 作为 State Owner 推进到 Running，方可接收玩家进入。
    // 状态机不允许 Creating 直跳 Running（见 scene_test.cpp:107），须经 Loading 过渡。
    auto ns = scenes_.Find(new_sid);
    if (ns) {
        (void)ns.Value()->TransitionTo(mmo::game::SceneState::Loading);
        (void)ns.Value()->TransitionTo(mmo::game::SceneState::Running);
    }
    shards.push_back(new_sid);
    return core::Result<mmo::game::SceneId>::Ok(new_sid);
}

core::Result<void> WorldManager::TransferPlayer(mmo::game::PlayerId player,
                                                 mmo::game::SceneId from,
                                                 mmo::game::SceneId to,
                                                 core::TraceID /*trace*/) {
    if (from == to) return core::Result<void>::Ok();  // 同场景，幂等

    auto fres = scenes_.Find(from);
    if (!fres) {
        return core::Result<void>::Fail(
            Error(ErrorCode::NOT_FOUND, "from scene missing", domain::kCore));
    }
    auto tres = scenes_.Find(to);
    if (!tres) {
        return core::Result<void>::Fail(
            Error(ErrorCode::NOT_FOUND, "to scene missing", domain::kCore));
    }

    // 目标满（仅 OpenWorld 分线有阈值；副本容量由 InstanceManager 管）：返回 BUSY，
    // 玩家留在原场景（§19 Failure）。
    if (tres.Value()->Type() == SceneType::World
        && tres.Value()->PlayerCount() >= cfg_.sharding_threshold) {
        return core::Result<void>::Fail(
            Error(ErrorCode::BUSY, "target shard full", domain::kCore));
    }

    // 离开原场景（释放 Avatar，延迟销毁，由 FlushDeferred 回收）。
    auto lres = fres.Value()->Leave(player, LeaveReason::Transfer);
    if (!lres) return core::Result<void>::Fail(lres.Err());

    // 进入目标场景：新建 Avatar（生命周期由 GameNode 统一；本模块只做逻辑转移）。
    auto e = entities_.Create(mmo::game::EntityType::Player, to,
                              mmo::game::Position{0.0f, 0.0f, 0.0f, 0.0f});
    if (!e) return core::Result<void>::Fail(e.Err());

    auto ires = tres.Value()->Enter(player, e.Value()->Id());
    if (!ires) {
        // 进入失败（极小概率）：尽量回滚到原场景，避免玩家悬空。
        (void)fres.Value()->Enter(player, e.Value()->Id());
        return core::Result<void>::Fail(ires.Err());
    }
    player_scene_[player] = to;
    return core::Result<void>::Ok();
}

core::Result<void> WorldManager::Tick(core::SteadyTime now) {
    now_ = now;
    (void)scenes_.TickAll(now);   // 驱动所有 Scene（含实例 Scene）
    (void)instances_.Tick(now);   // 实例回收
    return core::Result<void>::Ok();
}

mmo::game::SceneId WorldManager::PlayerScene(mmo::game::PlayerId player) const noexcept {
    auto it = player_scene_.find(player);
    return it == player_scene_.end() ? mmo::game::SceneId{0} : it->second;
}

}  // namespace mmo::game::world
