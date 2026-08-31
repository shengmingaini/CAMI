#pragma once

/// TASK-012 · SceneContext —— 传给各系统的运行期上下文（§7 / §15.3）。
///
/// 设计铁律（§21）：系统**只通过 Context 访问**共享系统，禁止反向持有 Scene 私有成员
/// 或缓存 Scene 指针。Context 聚合的是引用，Tick 结束即失效，禁止跨 Tick 保存。

#include <cstdint>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_manager.h"  // EntityManager（§27.2）
#include "mmo/game/scene/scene.h"            // SceneType
#include "mmo/game/scene/scene_id.h"         // SceneId / NodeId

namespace mmo::game {

struct SceneContext {
    SceneId id{0};
    SceneType type{SceneType::World};
    NodeId owner_node{0};
    core::SteadyTime now{};        // 当前 Tick 单调时刻（core::MonotonicClock::Point）
    std::uint64_t tick_number{0};  // 由 Scene 提供的当前 Tick 序号
    EntityManager& entities;       // 实体系统（§27.2 消费实体模块公开接口）
    core::EventBus& events;        // 事件总线（TASK-007）
    core::Scheduler& scheduler;    // 定时器调度器（TASK-004）
    core::Arena& frame_arena;      // 帧内 bump 分配，Tick 结束自动 Reset（TASK-004）

    SceneContext(SceneId id_, SceneType type_, NodeId owner_, core::SteadyTime now_,
                 std::uint64_t tick, EntityManager& e, core::EventBus& ev,
                 core::Scheduler& sch, core::Arena& ar)
        : id(id_), type(type_), owner_node(owner_), now(now_), tick_number(tick),
          entities(e), events(ev), scheduler(sch), frame_arena(ar) {}
};

}  // namespace mmo::game
