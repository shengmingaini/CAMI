#pragma once

/// TASK-037 §7 / §15.8-15.9 / §37.4 — Scene Recovery（第一版）。
///
/// 职责：从最近一次 Checkpoint 在「新 Owner GameNode」重建 Scene 容器。
/// 第一版明确**不做 Live Scene Migration**（保留实时状态迁移）—— 那属于第二阶段
/// （已登记 RFC：docs/rfc/scene-live-migration.md）。恢复后玩家可能回退到最近一次
/// Checkpoint（默认 30s 一次），这是第一版可接受的设计（TASK-037 §1 / §8）。
///
/// 模块边界（§27.3）：本头只依赖 TASK-012 Scene 的公开接口（scene.h / scene_manager.h），
/// 不 include 任何 src/；不直接连 MySQL / Redis（持久化经 DataService，TASK-026/027）。
///
/// 恢复职责划分：
///   - SceneRecovery 负责「Scene 容器（身份 + 版本 + 哈希 + 计数）在新节点重建」；
///   - 玩家实际状态（背包 / 属性 / 位置）的恢复由 Gateway Failover（37.3）+ Reconnect（37.2）
///     经 DataService 载入最近可靠持久化数据完成，不在本模块内（避免双写，§4 State Owner）。

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/core/log/trace_id.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/scene/scene.h"         // Scene / SceneType / SceneState（公开接口）
#include "mmo/game/scene/scene_id.h"      // SceneId / NodeId / PlayerId

namespace mmo::game::scene::recovery {

/// 一次可靠快照记录：Checkpoint 捕获的 Scene 权威元数据。
/// 第一版不含全部实体状态，只存「身份 + 版本 + 哈希 + 计数」（§8：只存玩家与关键 NPC，不存全部实体）。
struct SceneCheckpoint {
    SceneId       scene_id{0};
    SceneType     type{SceneType::World};
    NodeId        owner_at_checkpoint{0};           // 做 Checkpoint 时的 Owner（用于审计）
    std::uint32_t scene_version{0};                 // 快照时的 SceneVersion
    std::uint64_t tick_number{0};                   // TickNumber
    std::uint64_t state_hash{0};                    // StateHash（对账 / 防篡改）
    SceneState    scene_state{SceneState::Creating};
    std::size_t   player_count{0};                  // 快照时在线玩家数（恢复后回退到 0，待重连补回）
    std::size_t   entity_count{0};                  // 快照时实体数
    std::uint32_t checkpoint_version{0};            // 该 Scene 的第几次 checkpoint（1-based）
    core::SteadyTime taken_at{};
};

/// 工厂窄接口：解耦 SceneManager（§27.3 消费其公开 Create 接口）。
/// SceneRecovery 不直接持有 SceneManager，便于单测注入假工厂。
class ISceneFactory {
public:
    virtual ~ISceneFactory() = default;

    /// 在新 owner 节点上创建一个 Scene（用于恢复）。失败原样返回错误。
    virtual core::Result<Scene*> Create(SceneId id, SceneType type,
                                        NodeId new_owner) = 0;
};

/// Scene 恢复器：Checkpoint 采集 + 新节点重建。
///
/// 线程模型（§9）：由目标 GameNode 的 Simulation 线程调用，单写者；内部 store_ 用互斥保护
/// 以支持 Failover 协程并发查询 LastCheckpointVersion。
class SceneRecovery {
public:
    explicit SceneRecovery(ISceneFactory& factory, core::EventBus* bus = nullptr);

    /// 定期可靠快照：捕获 Scene 公开状态，写入 store_ 并 bump checkpoint_version。
    /// 失败（如存储不可用）时**保留上一个 checkpoint 并告警**，绝不以半成品覆盖（§19 Failure）。
    core::Result<void> Checkpoint(const Scene& s);

    /// 从最近 Checkpoint 在 new_owner 节点重建 Scene 容器。
    /// 无 checkpoint → NOT_FOUND；成功后新 Scene 被推到 Running（玩家随后经 Failover 重连补回）。
    core::Result<Scene*> Restore(SceneId id, NodeId new_owner, core::TraceID trace);

    /// 该 Scene 最近一次 checkpoint 的序号（0 = 从未 checkpoint）。
    std::uint32_t LastCheckpointVersion(SceneId id) const noexcept;

    // ---- 测试 / 运维钩子 ----

    /// 注入下一次 Checkpoint 失败（模拟存储写入失败，用于 §19 Failure 测试）。
    void FailNextCheckpoint(bool fail) noexcept;

    /// 当前持有的 checkpoint 数（对账）。
    std::size_t CheckpointCount() const noexcept;

private:
    ISceneFactory&            factory_;
    core::EventBus*           bus_;
    bool                      fail_next_{false};
    mutable std::mutex        mu_;
    std::unordered_map<SceneId, SceneCheckpoint> store_;
};

}  // namespace mmo::game::scene::recovery
