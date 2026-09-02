# QuestSystem · DEPENDENCY

## 上游依赖（仅消费公开接口，§27.2）

| 任务 | 模块 | 消费接口 |
|---|---|---|
| TASK-007 | `engine/core` | `core::EventBus`（Subscribe/Publish/PublishImmediate/Drain）、`core::Result` / `core::Error` / `core::ErrorCode`、`core::TraceID`、`core::MonotonicClock` / `SteadyTime` |
| TASK-016 | `server/gamenode/role` | 玩家等级经 `IPlayerQuery` 抽象读取（Quest 不持有等级副本，State Owner 归 Role） |
| TASK-018 | `server/gamenode/ai` | 击杀事件源：经本模块 `quest_events.h` 的 `MonsterKilled` 契约消费（AI 死亡时发布） |

`ItemCollected` / `NpcTalked` / `LocationReached` 的生产者（Inventory / Scene / Movement）
目前尚未实现对应模块，契约已由本模块在 `include/` 下定义并导出，下游直接发布即可被消费。

## 模块边界红线（§27.3）

- 代码只落在 `server/gamenode/quest/` 子树（include/ + src/ + tests/ + benchmark/ + docs/）。
- 下游只能 `#include` 本模块 `include/` 下的公开头；禁止 `#include` 本模块 `src/`。
- 禁止 `#include` 依赖模块的 `src/`（只消费其 `include/`）。
- 依赖方向单向（Game → Gameplay → Core），无循环依赖。
- 公开头不得泄露内部 `src/`（`scripts/verify` 静态扫描 `include/**` 是否引用 `src/`）。

## 被谁依赖

下游 Achievement / Social / Economy（TASK-029+）将消费 `QuestCompleted` 事件与 `IRewardSink`
实际发奖实现；QuestGiver / Scene 触发点负责调用 `Accept` / `TurnIn`。
