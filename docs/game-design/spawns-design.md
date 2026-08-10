# 刷新 / 种群 架构设计 (Spawns / Population)

> **文档状态**: [DESIGN]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-10  
> **定位**: 用数据驱动建模 WoW 级生物分布——刷新点、刷新池（池内按权重出怪）、刷新条件（等级/相位/事件/时段）、密度平衡。直接连经济源：击杀 = 掉落源。  
> **配套契约**: `proto/protobuf/config_spawns.proto`（config-center 热更，`*ConfigSet{data_version}`）  
> **联动**: `config_zones`（落点边界）、`config_creatures`（生物模板）、`config_world_events`（事件期改写）、`config_loot`（掉落源）。  
> **平衡校验**: 见 §5，密度 ↔ 经济源汇对账（核心经济平衡抓手）。

---

## 1. 设计原则

1. **落于区域边界内**：每个 `SpawnEntry.zone_id` 指向 `ZoneConfig`，`position` 必须落在 `bounds_min/max` 内——区域模块化边界直接约束刷新落点。
2. **池化控制多样性**：`SpawnPool` 用权重 + `max_active` 实现 WoW 式"池内互斥出怪"，避免单一怪物刷满。
3. **条件化可见**：`SpawnCondition` 支持等级带、相位、世界事件、时段——同一区域不同时间/进度呈现不同生态。
4. **密度即经济源**：单位时间击杀量决定掉落产出，是 `config_balance.EconomyFlow` 的**上游**。密度必须和经济源汇一起调，否则通胀/短缺。

---

## 2. 契约结构（config_spawns.proto）

| 消息 | 作用 | 关键字段 |
|------|------|---------|
| `SpawnCondition` | 出现条件 | min/max_level、required_phase_group_id、required_event_ids、time_of_day 窗口 |
| `SpawnEntry` | 刷新点 | spawn_id / zone_id / creature_template_id / position / wander_radius / respawn_sec / max_alive / group_size / condition / pool_id |
| `SpawnPoolEntry` | 池成员 | spawn_id / weight |
| `SpawnPool` | 刷新池 | pool_id / max_active / entries |
| `SpawnDensityBalance` | 密度平衡 | zone_id / target_kills_per_player_hour / tolerance_pct / alert_channel |
| `SpawnConfigSet` | 加载/热更单元 | data_version / spawns / pools / density |

**跨集引用裸 `uint32`**（防环）：zone_id→`config_zones`；creature_template_id→`config_creatures`；SpawnCondition.required_event_ids→`config_world_events`。仅 `import "common.proto"` 复用 `Vec3`。

---

## 3. 联动关系

```
[ZoneConfig.bounds_*] --约束落点--> [SpawnEntry.position]
[CreatureTemplate] <--creature_template_id-- [SpawnEntry] --respawn/max_alive--> [运行时 Scene 实例]
[WorldEvent] --required_event_ids--> [SpawnCondition]  (事件期临时出怪)
[SpawnEntry] --击杀--> [LootTable(config_loot)] --掉落--> [经济源(EconomyFlow)]
```

---

## 4. 运行时映射

| 契约 | 运行时 |
|------|--------|
| `SpawnEntry` | Scene 实例启动时按 `zone_id` 装载本区 Spawn；tick 内按 `respawn_sec` 重生、`max_alive` 限流（see `scene.md §3.2 spawnEntity`） |
| `SpawnPool` | Spawn 管理器维护池计数，达到 `max_active` 暂停池内其他 entry 出怪；按 `weight` 抽签 |
| `SpawnCondition` | 玩家进入/相位切换/事件开始时重算可见性（AOI + phasing） |
| `wander_radius` | 导航/碰撞查询（see `scene.md §3.4`） |

---

## 5. 平衡校验路径

| 编号 | 校验对象 | 方法 | 验证路径 |
|------|---------|------|---------|
| V1 | 密度 ↔ 经济源 | 离线：Σ(zone 内 spawn 击杀率) × `config_loot` 掉率 ≈ 该等级带 `EconomyFlow` 预期注入；偏差 > tolerance 告警 | `SpawnDensityBalance.target_kills_per_player_hour` 与 `config_balance.EconomyFlow` 对账 |
| V2 | 重生/存活上限 | 断言 `respawn_sec × 并发玩家` 不超出 Scene 容量预算（ADR-012/15 背压） | 容量规划器读 `SpawnConfigSet` |
| V3 | 池多样性 | 蒙特卡洛抽样池出怪分布，断言各 entry 实际占比 ∈ [权重 ± tolerance] | 离线模拟器 |

---

## 6. 开放问题 / 后续

- 区域级 `max_alive` 与全局 `SpawnPool.max_active` 的叠加语义需明确（先池后区 or 先区后池）。
- 动态密度（按在线人数弹性调 `max_alive`）属运行期策略，schema 暂留静态值。
- 与 WorldEvent 的"事件密度曲线"扩展待做（§5 V3 事件维度）。
