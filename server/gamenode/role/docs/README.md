# TASK-016 · Player / Character

模块路径：`server/gamenode/role`（`include/` + `src/` + `tests/` + `benchmark/` + `docs/`）

## 1. 职责与边界

**负责**：角色数据（等级 / 经验 / HP / MP / 属性 / 状态标志）、属性三层模型的派生计算、
配置化升级曲线、HP/MP 钳制与死亡事件、异步存档投递。

**不负责**（§4 / §21）：Scene / AOI / 网络 / MySQL。

- 场景实体绑定只记录 `avatar` / `scene` 两个 id，**不反向持有 Scene 对象**；
- 落盘只经 `IPersistenceAdapter` 异步投递，Tick 内绝不等待（§11）；
- 实时 HP/MP 的权威在 Scene / Combat（TASK-022），本模块的 `hp` / `mp` 是 Role 侧镜像，
  只按 `MaxHp` / `MaxMp` 做钳制与事件发布，禁止当作跨模块权威（§4 单写入者约束）。

## 2. 目录

```
server/gamenode/role/
├── include/mmo/game/role/
│   ├── attribute.h            属性三层模型 + Recompute（派生层私有）
│   ├── character.h            Character 结构
│   ├── exp_curve.h            配置化升级曲线（禁止默认构造）
│   ├── persistence_adapter.h  异步存档抽象 + 内存实现
│   ├── role_events.h          5 个角色事件（均 ≤32B）
│   └── role_system.h          RoleSystem 公开接口
├── src/                       实现
├── tests/role_test.cpp        §16 单元 / §17 集成 / §19 失败路径（ctest: Role.Suite）
├── benchmark/role_bench.cpp   §18 指标 → bench/role.txt
└── docs/                      INTERFACE.md / PERFORMANCE.md / README.md
```

配置：`config/gameplay/exp_curve.json`（曲线数值的唯一来源，代码内禁止硬编码）。

## 3. 构建与验证

```bash
export PATH=/c/msys64/mingw64/bin:$PATH

# 编译（Debug + Release）
cmake --build build/Release --target mmo_gamenode_role role_test role_bench
cmake --build build/Debug   --target mmo_gamenode_role role_test

# 单测（§16 / §17 / §19）
ctest --test-dir build/Release -R Role --output-on-failure

# Benchmark（§18）
./build/Release/bin/role_bench.exe --characters 1000   # → bench/role.txt
```

## 4. 关键设计决策

1. **派生层私有化**：`AttributeSet::total_` 是 `private`，外部只能 `Total()` 读、
   `Recompute()` 写 —— 「禁止任何系统直接改 Final 值」（§21）由编译器保证，不靠约定。
2. **派生属性的三层语义**：`Final = clamp(formula(主属性) + base + equipment + buff)`。
   与任务书 §8 同构，差别是把 `formula(主属性)` 视为隐含的第 0 层基底，
   使 `MaxHp` 真正随 `Stamina` 成长；装备/Buff 的「最大生命 +500」仍作为固定加成叠加。
3. **经验曲线走 `core::ConfigManager`**：复用 TASK-003 的配置体系与点号键路径
   （`exp_curve.base` 等），**不自带第二套 JSON 解析器**；`LoadFromFile` 幂等。
4. **禁止默认值静默启动**（§19）：`ExpCurve() = delete`；配置缺失/键缺失/数值非法一律报错。
5. **失败可恢复**：`Save` 失败进 `retry_queue_`（按角色去重）并**透传底层错误码**，
   后端恢复后 `FlushRetries()` 重投；Tick 全程不受影响。
6. **复活清除死亡标记**：HP 归零只发一次 `CharacterDied`；回血后清除，
   再次归零可重新触发（否则玩家第二次死亡将收不到事件）。
7. **「配置化」的分级**（对应验收项 2 的 grep 验证）：
   - **经验曲线**走外部 JSON（`config/gameplay/exp_curve.json` + `core::ConfigManager`），
     代码中**零硬编码**（`grep -rnE "\b(100|1\.5|60)\b" src/ include/` 只剩注释里的示例）。
   - **属性钳制区间与派生公式**是**进程级**配置，集中在 `attribute.cpp` 的
     `DefaultLimits()` / `AttrFormula` 单一事实来源，经 `SetAttrLimit` / `SetAttrFormula`
     在启动期注入覆盖——**不是散落在 `Recompute()` 里的魔数**，且有单测
     （`TestAttrLimitsAreConfigurable`）证明覆盖生效。
     不另开 `attribute.json` 的理由：属性表是静态的、不随部署变化的框架常量，
     而经验曲线是策划高频调优的数值；为前者引入「启动期必须先加载外部文件」的隐式契约，
     只会给下游 TASK-017 / 022 / 023 增加负担，收益为负。

## 5. 上游依赖（§27.2 / §27.3）

| 依赖 | 消费的公开接口 | 红线 |
|---|---|---|
| TASK-011 `gamenode/entity` | `EntityId` | 禁止 include 其 `src/` |
| TASK-012 `gamenode/scene` | `SceneContext` / `PlayerId` / `SceneId` | 同上；注意命名空间是 `mmo::game` 不是 `mmo::game::scene` |
| TASK-003 `engine/core` | `Result` / `EventBus` / `ConfigManager` / `TraceID` | — |

下游：TASK-017（装备系统写 `from_equipment`）、TASK-022（伤害系统复用 `HpChanged`）、
TASK-023（Buff 系统写 `from_buff`）、TASK-026（DataService 提供真实 `IPersistenceAdapter`）。

## 6. 实测结果摘要

| 指标 | 实测 | 预算 |
|---|---|---|
| `recompute_ns` | 22.6 | ≤ 300 |
| `add_exp_ns` | 36.1 | ≤ 100 |
| `modify_hp_ns` | 42.3 | ≤ 50 |
| `save_enqueue_ns` | 5.4 | ≤ 200 |
| `mem_bytes_per_character` | 496 | ≤ 512 |

集成：1000 角色 × 1000 Tick → Tick p50 8.2μs / p99 13.9μs；含 1000 次批量存档的 Tick 最差 21.0μs。
详见 `docs/PERFORMANCE.md`。
