# SkillSystem · DEPENDENCY

## 依赖方向（§27.3，只允许向下，禁止反向）

```
                 server/gamenode/combat  (TASK-021)
                            │
        ┌───────────────────┼───────────────────┬──────────────┐
        ▼                   ▼                   ▼              ▼
  engine/core          gamenode/entity    gamenode/role   gamenode/aoi   gamenode/movement
  ├ error (Result)     ├ EntityManager    ├ RoleSystem    └ IAoi         └ Distance3 / Vec3
  ├ bus (EventBus)     ├ Entity           └ Character
  ├ memory (Arena)
  ├ sched (Scheduler)
  ├ time (MonotonicClock / DurationMs)
  └ log (TraceID)
```

## 直接依赖

| 依赖 | 用途 | 门禁 |
|---|---|---|
| `engine/core` | `Result` 错误传播、`EventBus` 事件、`MonotonicClock` 时间、`TraceID` 链路 | 只读核心能力，无状态耦合 |
| `gamenode/entity` | `EntityManager` 查实体（坐标 / 类型 / 存在性）、`EntityId`、`EntityIndex` | §27.2 仅消费公开接口 |
| `gamenode/role` | `RoleSystem` 扣费与结算 HP / MP（`ModifyHp` / `ModifyMp` / `Find`）、`Character` 取 Attack | **禁止访问 Role 私有成员**（§4 State Owner） |
| `gamenode/aoi` | `IAoi::QueryVisible` 做 AOE 局部查询 | §10 / §21：禁止全 Scene 扫描 |
| `gamenode/movement` | `Distance3` 距离计算、`Vec3` 飞行物速度 | 仅数值工具 |

## 显式不依赖

- **不依赖** `gamenode/scene` 的场景管理逻辑：`SceneContext` 只作为参数传入（供 Tick 使用），
  模块自身不创建 / 销毁场景。
- **不依赖** `gamenode/quest` / `inventory` / `guild` 等玩法模块：跨模块联动一律经事件总线
  （发布 `DamageEvent` / `HealEvent` / `BuffApplied`），避免编译期耦合。
- **不依赖** 任何 `src/` 内部头（验收脚本红线扫描 `#include ...src/...`）。

## 被依赖方

当前无模块依赖 combat；后续 TASK-022（Movement 联动）、TASK-023（Buff 生效）将消费本模块事件。

## 关键约束

1. **线程模型**：技能状态只在 Scene 线程内写。`Update()` 由 Scene 同线程 Tick 驱动，模块内**不建线程**。
2. **事件为单向通知**：`Publish` 不入业务逻辑，订阅者不得回调 `SkillSystem` 修改状态（避免重入）。
3. **配置失败即失败**：技能 / Buff 加载失败返回 `Fail` 并由宿主决定是否拒绝启动，禁止用默认值静默兜底。
4. **零硬编码数值**：射程、冷却、消耗、伤害公式系数、飞行物速度 / 命中半径全部来自 JSON。
