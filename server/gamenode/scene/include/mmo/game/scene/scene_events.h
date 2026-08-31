#pragma once

/// TASK-012 · Scene 生命周期与玩家进出事件（§15.2 / §15.5）。
///
/// 全部走 EventBus（TASK-007）：Publish 仅入队，宿主线程 Drain 派发。
/// 事件为 POD / 纯值类型，满足 EventBus 内联预算（nothrow / alignof<=8 / <=32B）。

#include <cstdint>

#include "mmo/game/entity/entity_id.h"  // EntityId / SceneId
#include "mmo/game/scene/scene.h"       // SceneType / NodeId（scene.h 含 scene_id.h）

namespace mmo::game {

/// Scene 创建（SceneManager::Create 成功时由 Scene 构造发布）。
struct SceneCreated {
    SceneId id{0};
    SceneType type{SceneType::World};
    NodeId owner{0};
};

/// Scene 进入 Loading 状态。
struct SceneLoaded {
    SceneId id{0};
    SceneType type{SceneType::World};
    NodeId owner{0};
};

/// Scene 进入 Running 状态（可接纳玩家）。
struct SceneRunning {
    SceneId id{0};
    SceneType type{SceneType::World};
    NodeId owner{0};
};

/// Scene 进入 Draining 状态（拒绝新玩家，等待存量玩家离开）。
struct SceneDraining {
    SceneId id{0};
    SceneType type{SceneType::World};
    NodeId owner{0};
};

/// Scene 进入 Destroying 状态 / 被 SceneManager 销毁。
struct SceneDestroyed {
    SceneId id{0};
    SceneType type{SceneType::World};
    NodeId owner{0};
};

/// 玩家进入场景（Enter 成功时发布）。
struct ScenePlayerEntered {
    SceneId id{0};
    PlayerId player{0};
    EntityId avatar{0};
};

/// 玩家离开场景（Leave 成功时发布；Avatar 走延迟销毁）。
struct ScenePlayerLeft {
    SceneId id{0};
    PlayerId player{0};
    EntityId avatar{0};
    LeaveReason reason{LeaveReason::Disconnect};
};

}  // namespace mmo::game
