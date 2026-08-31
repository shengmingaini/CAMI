#pragma once

/// TASK-012 · SceneManager（§7 / §15.4 / §15.9 / §17）。
///
/// Scene 表用读写锁保护：创建/销毁走写锁，Tick 遍历走读锁取快照（§15.4）。
/// TickAll 额外用 tick_mu_ 单写者互斥，**禁止并发 Tick 同一 Scene**（§9 / §21）。
///
/// 模块边界（§27.3）：只调用依赖模块声明接口，禁止访问其内部数据。

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "mmo/core/bus/event_bus.h"
#include "mmo/core/error/result.h"
#include "mmo/core/memory/arena.h"
#include "mmo/core/sched/scheduler.h"
#include "mmo/core/time/clock.h"
#include "mmo/game/entity/entity_manager.h"
#include "mmo/game/scene/scene.h"
#include "mmo/game/scene/scene_context.h"
#include "mmo/game/scene/scene_id.h"

namespace mmo::game {

class SceneManager {
public:
    SceneManager(EntityManager& entities, core::EventBus& events,
                 core::Scheduler& scheduler, core::Arena& arena);

    SceneManager(const SceneManager&) = delete;
    SceneManager& operator=(const SceneManager&) = delete;

    // ---- Scene 表操作（§7）----
    /// 创建场景。Id 已存在返回 VERSION_CONFLICT（§19 并发创建同 Id）。
    core::Result<Scene*> Create(SceneId, SceneType, NodeId owner);
    /// 查找场景；不存在返回 NOT_FOUND。返回指针在 Destroy 前有效。
    core::Result<Scene*> Find(SceneId) noexcept;
    /// 销毁场景：移出表后于锁外发布 SceneDestroyed（§21 禁止 Tick 内同步销毁）。
    core::Result<void> Destroy(SceneId);
    /// 顺序遍历所有场景 Tick（§9：单 Owner，禁止并发 Tick 同一 Scene）。
    core::Result<void> TickAll(core::SteadyTime now);
    std::vector<Scene*> All() const;
    std::size_t Count() const noexcept;

private:
    // 共享系统引用（§27.2）
    EntityManager& entities_;
    core::EventBus& events_;
    core::Scheduler& scheduler_;
    core::Arena& arena_;

    mutable std::shared_mutex scenes_mu_;  // 创建/销毁写，遍历读
    std::unordered_map<SceneId, std::unique_ptr<Scene>> scenes_;
    std::mutex tick_mu_;  // 序列化 TickAll 与 Destroy，禁止并发 Tick / 中途销毁同一 Scene
};

}  // namespace mmo::game
