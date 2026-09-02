# AiSystem · DEPENDENCY

## 上游依赖（仅消费公开接口，§27.2）

| 任务 | 模块 | 消费接口 |
|---|---|---|
| TASK-011 | `server/gamenode/entity` | `EntityManager`（Create/Destroy/Find/Each）、`EntityType`、`EntityId`、`Position` |
| TASK-014 | `server/gamenode/aoi` | `aoi::IAoi`（Enter/Leave/Move/QueryVisible）、`CreateDynamicGridAoi` |
| TASK-015 | `server/gamenode/movement` | `movement::MovementSystem`（Register/SetVelocity/StateOf/Integrate） |
| TASK-012 | `server/gamenode/scene` | `scene::SceneContext`（entities/scheduler/now 等聚合引用） |
| TASK-004 | `engine/core` | `core::Scheduler`（ScheduleAfter/Tick，不建线程） |
| TASK-008 | `engine/core` | `core::MonotonicClock`、`core::SteadyTime`、`core::DurationMs` |

## 模块边界红线（§27.3）

- 代码只落在 `server/gamenode/ai/` 子树（include/ + src/ + tests/ + benchmark/ + docs/）。
- 下游只能 `#include` 本模块 `include/` 下的公开头；禁止 `#include` 本模块 `src/`。
- 禁止 `#include` 依赖模块的 `src/`（只消费其 `include/`）。
- 禁止访问依赖模块内部数据；依赖方向单向（Game → Gameplay → Core），无循环依赖。
- 公开头不得泄露内部 `src/`（`scripts/verify` 静态扫描）。

## 被谁依赖

下游 Combat / Quest 等模块将消费 `AiSystem` 的 `OnDamaged` / `StateOf` 等接口。
