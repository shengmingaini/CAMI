# World / Instance · 公开接口（TASK-020）

> 本文件描述 `server/gamenode/world/include/` 下冻结的对外契约。接口一旦 `STATUS: DONE`
> 即视为冻结，下游依赖它；破坏性变更须走 `version` + 兼容性评估（§27.1 / §27.3）。

## 命名空间

`mmo::game::world`。类型归属提醒：`SceneId / PlayerId / NodeId / LeaveReason` 定义在
`mmo::game`（**非** `mmo::game::scene`）；`SpawnDef` 在 `mmo::game::ai`。

## 公共类型（`instance_def.h`）

| 类型 | 说明 |
|---|---|
| `InstanceState : uint8_t` | `Pending / Loading / Running / Completed / Destroying` |
| `InstanceType : uint8_t` | `OpenWorld / Dungeon / Arena / Battleground / TemporaryInstance`（数值与 SceneType 对齐） |
| `InstanceResult : uint8_t` | `Cleared / Failed / Abandoned / Timeout` |
| `InstanceId = uint64_t` | 实例全局唯一 ID（单调递增，永不复用） |
| `WorldConfig` | `sharding_threshold`(默认 300) / `empty_instance_timeout`(默认 300s) / `destroy_grace`(默认 60s) |
| `InstanceDef` | 实例静态定义（全部配置化，`name`/`scene_asset` 为 `string_view`，指向加载器字符串池） |
| `Instance` | 实例运行时快照（对外字段 + 内部回收簿记字段，不破坏对外语义） |

## InstanceManager（`instance_manager.h`）

```cpp
core::Result<InstanceId> Create(uint32_t def_id, std::span<const PlayerId> members, core::TraceID);
core::Result<void>    Start(InstanceId, core::TraceID);              // Pending→Loading→Running
core::Result<void>    Complete(InstanceId, InstanceResult, core::TraceID);  // →Completed
core::Result<void>    Destroy(InstanceId, core::TraceID);           // 立即回收
core::Result<void>    AddMember(InstanceId, PlayerId, core::TraceID);   // 仅 Loading/Running；超上限 BUSY；重复 INVALID_ARGUMENT
core::Result<void>    RemoveMember(InstanceId, PlayerId, LeaveReason, core::TraceID);
core::Result<void>    Tick(core::SteadyTime now);                    // 回收 + 分批析构（≤10/Tick）
const Instance*       Find(InstanceId) const noexcept;
std::size_t          CountByState(InstanceState) const noexcept;
```

构造：`InstanceManager(SceneManager&, ai::AiSystem&, EntityManager&, EventBus&, Scheduler&, Arena&, NodeId)`。
加载配置：`LoadConfig(dir)`；回收策略：`SetReclaimPolicy(empty_timeout, grace)`。

## WorldManager（`world_manager.h`）

```cpp
core::Result<void>            Init(const WorldConfig&);
core::Result<void>            LoadConfig(std::string_view dir);
core::Result<SceneId>         GetOrCreateOpenWorld(uint32_t world_def_id);  // 分线：全满分线则新建
core::Result<void>            TransferPlayer(PlayerId, SceneId from, SceneId to, core::TraceID);  // 走 Enter/Leave，禁搬实体；满则 BUSY
core::Result<void>            Tick(core::SteadyTime now);                  // 驱动 Scene + 实例回收
std::size_t                   PlayerCount() const noexcept;
std::size_t                   SceneCount()  const noexcept;
SceneId                       PlayerScene(PlayerId) const noexcept;
```

## 状态机（§8）

```
Pending ──> Loading ──> Running ──> Completed ──> Destroying
   │            │           │
   └────────────┴───────────┴──> (超时 / 全员退出 / 创建失败) Destroying
```

- OpenWorld：常驻，不自动销毁；单 Scene 超 `sharding_threshold` 自动开分线。
- Dungeon/Arena：全员退出或超时后进入 Destroying（延迟 `destroy_grace` 秒兜底）。
- 空实例（创建后从未进入 > `empty_instance_timeout`）自动回收。
- 单 Tick 回收上限 10（防尖峰，§19 / §21 Forbidden）。
