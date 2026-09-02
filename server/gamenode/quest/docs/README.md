# QuestSystem · README

事件驱动的 MMORPG 任务系统（TASK-019，Phase 4 基础 MMORPG）。

## 范围

- 接取 / 放弃 / 交还（Accept / Abandon / TurnIn）生命周期。
- 四类事件驱动进度：击杀怪物（MonsterKilled）、拾取物品（ItemCollected）、
  与 NPC 对话（NpcTalked）、到达区域（LocationReached）。
- 倒排索引：事件到达 O(1) 定位相关任务，**绝不遍历所有玩家**（§21 反模式红线）。
- 多目标、前置任务链（prerequisites）、等级门槛（经 `IPlayerQuery`）。
- 交还幂等：重复交任务只发一次奖励；发奖失败保持 Completed，可安全重试。
- 任务定义全部配置化（`config/gameplay/quests/*.json`），代码无硬编码数值。

## 不在本任务范围

- Economy 实际发奖（TASK-029 未就绪）：奖励经 `IRewardSink` 幂等占位接口下发。
- 任务追踪 UI / 网络 RPC / 持久化（§11/§13，由上游网关与存档模块承担）。
- 自动接取 / 自动交还（仅提供 API，触发点归 Scene / QuestGiver）。

## 目录

```
include/mmo/game/quest/   quest_system.h / quest_def.h / quest_events.h
                          quest_index.h / quest_instance.h / quest_config.h
src/                      quest_system.cpp / quest_index.cpp / quest_config.cpp
tests/                    quest_test.cpp
benchmark/                quest_bench.cpp
config/gameplay/quests/   quests.json（5 条示例任务）
docs/                     INTERFACE / PERFORMANCE / README / DEPENDENCY / TEST
```
