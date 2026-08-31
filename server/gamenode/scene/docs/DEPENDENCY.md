# Scene System · DEPENDENCY（TASK-012）

## 前置任务

| 任务 | 关系 | 消费接口 |
|---|---|---|
| TASK-011 Entity System | 硬依赖（门禁 `require_tasks_done 011`） | `EntityManager` / `EntityId` / `SceneId`（均 `include/` 公开） |

## 依赖方向（§27.3，单向 Game → Gameplay → Core）

```
server/gamenode/scene
   ├── engine/core : error / bus / memory / sched / time
   └── server/gamenode/entity (TASK-011) : EntityManager / EntityId / SceneId
```

- **禁止循环依赖**：Scene 不反向被 entity 依赖；entity 不知道 Scene 存在。
- **禁止跨模块内部数据**：只调 `include/` 声明接口，不 `#include` 上游 `src/`。

## 链接的 core 库别名

`mmo::core_error` `mmo::core_bus` `mmo::core_memory` `mmo::core_sched` `mmo::core_time`
+ `mmo::gamenode_entity`（TASK-011 实体库别名）。

## 不依赖的模块（模块边界红线）

- **gateway**：`PlayerId` / `NodeId` 在 gateway 另有定义，但 Scene 模块**不依赖 gateway**
  （避免跨子树耦合）；Scene 自有同义 `using` 类型，互不冲突。
- **Redis / MySQL / gRPC / Kafka**：Scene 热路径（§10/§11/§12/§13 均为 NO）不触碰任何外部 IO；
  Redis 仅作非权威副本，绝不作为权威数据源（§21）。
