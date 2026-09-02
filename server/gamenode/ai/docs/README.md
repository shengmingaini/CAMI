# AiSystem · README

NPC / Monster 第一版 AI 状态机（TASK-018，Phase 4 基础 MMORPG）。

## 范围

- 六态状态机：Idle / Patrol / Chase / Attack / Return / Dead。
- 生成 / 销毁（Spawn / Despawn），与 EntityManager / AOI / Movement 生命周期联动。
- 表驱动状态转移（禁止散落 if）。
- 决策节流 5Hz（禁止每 Tick 全量决策）。
- 局部目标选取（AOI QueryVisible，禁止全 Scene 扫描）。
- 死亡 + Scheduler 重生（不建线程）。
- NPC / Monster 属性全部配置化（`config/gameplay/npc/*.json`）。

## 不在本任务范围

- 行为树（先状态机，§21）。
- Combat 伤害结算（AI 仅持有血量镜像用于演示死亡/重生；权威血量归 Combat）。
- 网络 RPC / 持久化 / 外部 IO（§11/§13）。

## 目录

```
include/mmo/game/ai/   ai_state.h / ai_system.h / spawn_def.h / ai_component.h / npc_config.h
src/                   ai_state.cpp / ai_system.cpp / npc_config.cpp
tests/                 ai_test.cpp
benchmark/             ai_bench.cpp
config/gameplay/npc/   monsters.json / npcs.json
docs/                  INTERFACE / PERFORMANCE / README / DEPENDENCY / TEST
```
