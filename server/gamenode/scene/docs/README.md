# Scene System · README（TASK-012）

> 实时仿真基本单位。Phase 3 GameNode Core 的第二个子树（首个为 TASK-011 Entity System）。
> 依赖：TASK-011 EntityManager（创建 / 延迟销毁 Avatar 实体）。

## 职责

- **Scene** 是实时状态的权威 Owner（§4）：只有持有该 Scene 的 GameNode Simulation 线程可写；
  Redis / MySQL 只持有非权威副本，禁止作为权威数据源（§21）。
- 管理场景生命周期（五状态机）、玩家进出、增量 StateHash、Tick 驱动。

## 模块边界（§27.3）

- 代码只落在 `server/gamenode/scene/`（include / src / tests / docs）。
- 下游只能调用本模块 `include/` 公开接口，禁止 `#include` 本模块 `src/` 或内部头。
- 只消费 TASK-011 实体模块 `include/` 公开接口（EntityManager / EntityId / SceneId），禁止访问其内部数据。
- `SceneId` **复用** TASK-011 实体模块定义（uint64），不重复定义第二套（实体模块明确禁止）。

## 文件布局

| 路径 | 说明 |
|---|---|
| `include/mmo/game/scene/scene_id.h` | SceneId 编码辅助 + PlayerId / NodeId / LeaveReason（无第二套 SceneId） |
| `include/mmo/game/scene/scene.h` | Scene 类、五状态机、Enter/Leave/Tick/ComputeStateHash |
| `include/mmo/game/scene/scene_context.h` | SceneContext（系统只通过它访问，禁止反向持有 Scene） |
| `include/mmo/game/scene/scene_events.h` | SceneCreated/Loaded/Running/Draining/Destroyed/PlayerEntered/Left |
| `include/mmo/game/scene/scene_manager.h` | SceneManager（读写锁表 + TickAll 序列化） |
| `src/scene.cpp` `src/scene_manager.cpp` | 实现 |
| `tests/scene_test.cpp` `tests/scene_bench.cpp` | 单元/集成/失败测试 + Benchmark |

## 构建与验收

```bash
bash scripts/verify/task-012.sh          # Debug+Release 双构建 + ctest Scene + bench + 断言
bash scripts/task-done.sh TASK-012       # 翻转 STATUS: DONE（验收 exit 0 后才能跑）
```

详见 `INTERFACE.md` / `DEPENDENCY.md` / `PERFORMANCE.md` / `TEST.md`。
