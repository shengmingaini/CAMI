#pragma once

/// TASK-013 · ISimulationStage —— 单阶段抽象（§7）。
///
/// 每个阶段（Input / Movement / AOI / Combat / Buff / Quest / Event / Replication）
/// 实现一个 ISimulationStage，由 SimulationScheduler 按固定顺序在每 Tick 驱动一次。
///
/// 设计铁律（§9 / §10 / §21）：
///   - Execute 期间不得阻塞、不得做 IO（禁止 MySQL / Redis / gRPC / 文件 / 网络）。
///   - 禁止在 Execute 内大规模内存分配（走帧 Arena / 对象池）。
///   - 单阶段抛异常由 Scheduler 捕获并跳过，禁止拖垮整个 Tick（§19）。
///   - 阶段只经 SceneContext 访问共享系统，禁止反向持有 Scene/Scheduler 指针。

#include <memory>
#include <string_view>

#include "mmo/game/sched/tick_phase.h"
#include "mmo/game/scene/scene_context.h"  // SceneContext（§27.2 消费 TASK-012 公开接口）

namespace mmo::game {

class ISimulationStage {
public:
    virtual ~ISimulationStage() = default;

    /// 该阶段所属 TickPhase（用于排序与计时分区）。
    virtual TickPhase Phase() const noexcept = 0;

    /// 执行该阶段逻辑。ctx 为本 Tick 的运行期上下文（聚合引用，Tick 结束即失效）。
    /// 禁止在此阻塞（§9）。
    virtual void Execute(const SceneContext& ctx) = 0;

    /// 阶段可读名（日志 / 调试）。
    virtual std::string_view Name() const noexcept = 0;
};

}  // namespace mmo::game
