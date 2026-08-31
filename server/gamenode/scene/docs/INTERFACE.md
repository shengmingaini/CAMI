# Scene System · INTERFACE（TASK-012）

> 冻结契约（§27.1）：`STATUS: DONE` 后签名不可破坏性变更；扩展走 ID 段 / 注册表（§27.4）。

## 公开类型（namespace `mmo::game`）

```cpp
enum class SceneState : uint8_t { Creating, Loading, Running, Draining, Destroying };
enum class SceneType : uint8_t { World, Dungeon, Arena, Battleground, TemporaryInstance };
enum class LeaveReason : uint8_t { Disconnect, Logout, Kick, Transfer, Timeout };

using PlayerId = uint64_t;   // Scene 模块自有，不依赖 gateway
using NodeId   = uint32_t;   // 拥有该 Scene 的 GameNode
// SceneId 复用 TASK-011 mmo::game::SceneId (uint64)，禁止重定义
```

## Scene

```cpp
class Scene {
    Scene(SceneId id, SceneType type, NodeId owner,
          EntityManager& entities, core::EventBus& events);

    SceneId Id() const noexcept;            // SceneID
    SceneType Type() const noexcept;
    SceneState State() const noexcept;
    uint32_t Version() const noexcept;       // SceneVersion（每次合法转移 +1）
    uint64_t TickNumber() const noexcept;    // TickNumber
    uint64_t StateHash() const noexcept;     // StateHash
    NodeId OwnerNode() const noexcept;       // OwnerGameNode

    core::Result<void> Enter(PlayerId, EntityId avatar);  // 仅 Running；超容量 BUSY；重复 INVALID_ARGUMENT
    core::Result<void> Leave(PlayerId, LeaveReason);       // 解绑 + 延迟销毁 Avatar
    core::Result<void> Tick(const SceneContext&);          // 由 SceneManager::TickAll 驱动
    size_t PlayerCount() const noexcept;
    size_t EntityCount() const noexcept;

    core::Result<void> TransitionTo(SceneState);  // 非法转移 INVALID_ARGUMENT
    core::Result<void> ComputeStateHash();        // 每 60 Tick 一次，确定性
};
```

## SceneContext（§15.3，禁止反向持有 Scene）

```cpp
struct SceneContext {
    SceneId id; SceneType type; NodeId owner_node;
    core::SteadyTime now; uint64_t tick_number;
    EntityManager& entities; core::EventBus& events;
    core::Scheduler& scheduler; core::Arena& frame_arena;
};
```

## SceneManager

```cpp
class SceneManager {
    SceneManager(EntityManager& entities, core::EventBus& events,
                 core::Scheduler& scheduler, core::Arena& arena);
    core::Result<Scene*> Create(SceneId, SceneType, NodeId owner);  // 同 Id → VERSION_CONFLICT
    core::Result<Scene*> Find(SceneId) noexcept;                    // 不存在 → NOT_FOUND
    core::Result<void> Destroy(SceneId);                           // 锁外发布 SceneDestroyed
    core::Result<void> TickAll(core::SteadyTime now);              // 单写者互斥
    std::vector<Scene*> All() const;
    size_t Count() const noexcept;
};
```

## 生命周期事件（走 EventBus，§15.2 / §15.5）

`SceneCreated` / `SceneLoaded` / `SceneRunning` / `SceneDraining` / `SceneDestroyed`（均含 id/type/owner）
+ `ScenePlayerEntered{id,player,avatar}` / `ScenePlayerLeft{id,player,avatar,reason}`。
