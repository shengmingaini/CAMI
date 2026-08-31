#pragma once

/// TASK-011 · 实体生命周期事件（§6 / §15.6 / §17）。
///
/// 全部走 EventBus（TASK-007）：Publish 仅入队，由宿主线程 Drain 派发。
/// 事件为 POD / 纯值类型，满足 EventBus 要求：nothrow 析构 / nothrow 移动 /
/// alignof<=8 / <=32 字节内联（event_slot.h §内存预算）。

#include <cstdint>

#include "mmo/game/entity/entity_id.h"

namespace mmo::game {

/// 实体创建（在 Create 成功时发布）。
struct EntityCreated {
    EntityId id{0};
    EntityType type{EntityType::Player};
    SceneId scene{0};
};

/// 实体销毁（在 FlushDeferred 物理回收时发布，即 Tick 边界，§15.7）。
struct EntityDestroyed {
    EntityId id{0};
    EntityType type{EntityType::Player};
    SceneId scene{0};
};

/// 组件挂载（AddComponent 成功时发布）。
struct ComponentAttached {
    EntityId id{0};
    ComponentTypeId type{};
};

/// 组件卸载（RemoveComponent 成功时发布）。
struct ComponentDetached {
    EntityId id{0};
    ComponentTypeId type{};
};

}  // namespace mmo::game
