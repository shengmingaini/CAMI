#pragma once

/// TASK-020 · WorldManager 公开接口（§7，冻结契约）。
///
/// 职责：OpenWorld 常驻场景管理 + 分线（300 人阈值）+ 玩家跨场景转移（TransferPlayer
/// 走 Scene Enter/Leave，禁止直接搬实体，§15.6 / §21）+ 全局 Tick 驱动 + 指标。
///
/// 依赖（§27.2，仅消费公开接口）：TASK-012 SceneManager、本任务 InstanceManager、
/// TASK-011 EntityManager（TransferPlayer 重建 Avatar 实体）。
/// 不创建线程、不做数据库 / 网络访问（§10 / §11）。

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/scene_id.h"
#include "mmo/game/scene/scene_manager.h"
#include "mmo/game/world/instance_def.h"
#include "mmo/game/world/instance_manager.h"
#include "mmo/game/world/world_config.h"

namespace mmo::game::world {

/// 世界管理器（§7 / §15.6 ~ §15.8）。
class WorldManager {
public:
    WorldManager(SceneManager& scenes,
                 InstanceManager& instances,
                 EntityManager& entities,
                 NodeId owner_node) noexcept;

    WorldManager(const WorldManager&) = delete;
    WorldManager& operator=(const WorldManager&) = delete;

    // ---- 配置（§15.1 / §15.7） ----
    /// 注入分线 / 回收策略，并透传到 InstanceManager。
    core::Result<void> Init(const WorldConfig& cfg);
    /// 加载 OpenWorld 定义；失败返回错误。
    core::Result<void> LoadConfig(std::string_view dir);

    // ---- OpenWorld（§15.6） ----
    /// 取或建 OpenWorld 分线 Scene：存在未满分线则返回之；全满或尚无则新建分线。
    core::Result<mmo::game::SceneId> GetOrCreateOpenWorld(std::uint32_t world_def_id);

    // ---- 玩家转移（§15.6 / §19） ----
    /// 跨 Scene 转移：先校验目标容量（满则 BUSY，玩家留原场景），再 Leave(from)+Enter(to)。
    /// 禁止直接搬移实体（§21 Forbidden）。
    core::Result<void> TransferPlayer(mmo::game::PlayerId player,
                                       mmo::game::SceneId from,
                                       mmo::game::SceneId to,
                                       core::TraceID trace);

    // ---- Tick 与观测（§7 / §10） ----
    /// 驱动所有 Scene（含实例 Scene）+ 实例回收。now 用于回收时序判定。
    core::Result<void> Tick(core::SteadyTime now);

    std::size_t PlayerCount() const noexcept { return player_scene_.size(); }
    std::size_t SceneCount()  const noexcept { return scenes_.Count(); }

    /// 观测：玩家当前所在 Scene（未登记返回 0）。
    mmo::game::SceneId PlayerScene(mmo::game::PlayerId player) const noexcept;

private:
    SceneManager&    scenes_;
    InstanceManager& instances_;
    EntityManager&   entities_;
    NodeId           owner_node_;

    WorldConfig cfg_;
    std::unordered_map<std::uint32_t, WorldDef> world_defs_;
    std::unordered_map<std::uint32_t, std::vector<mmo::game::SceneId>> open_worlds_;
    std::unordered_map<mmo::game::PlayerId, mmo::game::SceneId> player_scene_;
    std::uint64_t world_scene_index_{0};
    core::SteadyTime now_{};
};

}  // namespace mmo::game::world
