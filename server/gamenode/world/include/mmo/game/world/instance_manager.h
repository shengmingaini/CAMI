#pragma once

/// TASK-020 · InstanceManager 公开接口（§7，冻结契约）。
///
/// 职责：副本实例生命周期（五状态机）+ 成员管理 + 超时/空实例/全员退出三条回收路径
/// + 单 Tick 分批回收（上限 10，防尖峰，§19 / §21 Forbidden）。
///
/// 依赖（§27.2，仅消费公开接口）：TASK-012 SceneManager（创建/销毁 Scene）、
/// TASK-018 AiSystem（Loading 阶段生成怪物/NPC）、TASK-011 EntityManager（Builder 引用）、
/// TASK-004 core::{EventBus,Scheduler,Arena}（构造 SceneContext 供 AI Spawn）。
/// 不创建线程、不做数据库 / 网络访问（§10 / §11）。

#include <cstdint>
#include <span>
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
#include "mmo/game/ai/ai_system.h"
#include "mmo/game/world/instance_def.h"
#include "mmo/game/world/world_config.h"

namespace mmo::game::world {

/// 副本实例管理器（§7 / §15.2 ~ §15.5）。
class InstanceManager {
public:
    /// 持有实例生命周期所需的共享系统引用（由 GameNode 构造时注入，禁止反向持有 Scene）。
    InstanceManager(SceneManager& scenes,
                   ai::AiSystem& ai,
                   EntityManager& entities,
                   core::EventBus& events,
                   core::Scheduler& scheduler,
                   core::Arena& arena,
                   NodeId owner_node) noexcept;

    InstanceManager(const InstanceManager&) = delete;
    InstanceManager& operator=(const InstanceManager&) = delete;

    // ---- 配置（§15.1 全配置化） ----
    /// 加载实例定义；失败返回错误，禁止静默默认（§19）。
    core::Result<void> LoadConfig(std::string_view dir);

    /// 设置回收策略（空实例超时 / 全员退出宽限）。测试可缩小以加速验证。
    void SetReclaimPolicy(core::DurationMs empty_instance_timeout,
                          core::DurationMs destroy_grace) noexcept;

    // ---- 生命周期（§7 / §8） ----
    /// 分配 ID + 记录定义 + 入 Pending。def_id 不存在返回 NOT_FOUND。
    core::Result<InstanceId> Create(std::uint32_t def_id,
                                     std::span<const mmo::game::PlayerId> members,
                                     core::TraceID trace);

    /// Pending → Loading（创建 Scene + 生成怪物/NPC）→ Running。
    /// Scene 创建失败转 Destroying（无悬挂 Scene，§19）。
    core::Result<void> Start(InstanceId id, core::TraceID trace);

    /// Running → Completed（按时宽限后回收）。
    core::Result<void> Complete(InstanceId id, InstanceResult result, core::TraceID trace);

    /// 立即回收：销毁 Scene + 移除实例（跳过宽限）。
    core::Result<void> Destroy(InstanceId id, core::TraceID trace);

    // ---- 成员管理（§15.5） ----
    /// 加入：仅 Loading/Running 可加；重复加入 INVALID_ARGUMENT；超 max_players 返回 BUSY。
    core::Result<void> AddMember(InstanceId id, mmo::game::PlayerId player, core::TraceID trace);

    /// 退出：移除成员；若成员清空且处于 Loading/Running/Completed，进入回收（宽限）。
    core::Result<void> RemoveMember(InstanceId id, mmo::game::PlayerId player,
                                     LeaveReason reason, core::TraceID trace);

    // ---- Tick 与观测（§7 / §10） ----
    /// 超时检查 / 空实例回收 / 全员退出回收 / 分批析构（单 Tick 上限 10）。
    core::Result<void> Tick(core::SteadyTime now);

    /// 查找实例；不存在返回 nullptr。指针在下次变更前有效。
    const Instance* Find(InstanceId id) const noexcept;

    /// 各状态实例数（指标）。
    std::size_t CountByState(InstanceState s) const noexcept;

    // ---- 指标 ----
    std::size_t Count()        const noexcept { return instances_.size(); }
    std::size_t CreateCount()  const noexcept { return create_count_; }
    std::size_t DestroyCount() const noexcept { return destroy_count_; }

private:
    SceneManager&   scenes_;
    ai::AiSystem&   ai_;
    EntityManager&  entities_;
    core::EventBus& events_;
    core::Scheduler& scheduler_;
    core::Arena&    arena_;
    NodeId          owner_node_;

    WorldConfigBundle bundle_;   // 拥有实例配置 + 字符串池（string_view 生命期）

    std::unordered_map<InstanceId, Instance> instances_;
    // Loading 阶段生成的怪物/NPC 实体 ID，回收时统一 Despawn（防泄漏，§17 集成）。
    std::unordered_map<InstanceId, std::vector<mmo::game::EntityId>> spawned_;
    InstanceId      next_id_{0};
    std::uint64_t   next_scene_index_{0};

    core::SteadyTime now_;
    core::DurationMs empty_timeout_{300'000};
    core::DurationMs destroy_grace_{60'000};

    std::size_t create_count_{0};
    std::size_t destroy_count_{0};

    static constexpr std::size_t kMaxReclaimPerTick = 10;  // §19 / §21 防尖峰

    const InstanceDef* LookupDef(std::uint32_t def_id) const;
    Instance*          Mutable(InstanceId id) noexcept;
    SceneType          SceneTypeFor(InstanceType t) const noexcept;
    core::SteadyTime   Now() const noexcept { return now_; }
};

}  // namespace mmo::game::world
