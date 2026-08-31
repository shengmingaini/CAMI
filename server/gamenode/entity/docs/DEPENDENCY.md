# Entity System · DEPENDENCY（TASK-011）

## 前置任务（依赖方向单向，禁止循环）
- **TASK-004 · Core Memory / Thread / Scheduler**
  - 消费：`engine/core/include/mmo/core/memory/object_pool.h`（`ObjectPool<Entity>` 实体池化）、`arena.h`（备留，本任务未直接用于实体，组件用稀疏数组）
  - 红线：只消费 `include/`，禁止 `#include` 其 `src/`
- **TASK-007 · Command / Query / Event Bus**
  - 消费：`engine/core/include/mmo/core/bus/event_bus.h`（`Publish`/`Drain`/`Subscribe`，生命周期事件异步派发）
  - 消费：`engine/core/include/mmo/core/error/{error,error_code,result}.h`（`Result`/`Error`/`ErrorCode`）

## 链接目标
`mmo::core_error` `mmo::core_bus` `mmo::core_memory`（见 `server/gamenode/entity/CMakeLists.txt`）

## 被谁依赖（下游，尚未落地）
- TASK-012 Scene、TASK-015 Movement、TASK-017 Inventory、TASK-018 AI、TASK-023 Buff、TASK-024 Combat —— 均通过本模块 `include/` 调用，组件定义在各任务自有子树。

## 模块边界
- 本模块只落在 `server/gamenode/entity`，不扩散到其它任务子树。
- 下游只能调用本模块 `include/` 公开接口，禁止 `#include src/`。
- 禁止访问依赖模块内部数据；禁止在 `STATUS: DONE` 后静默改签名。
