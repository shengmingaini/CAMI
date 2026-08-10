# 世界事件 / 节日 架构设计 (World Events / Holidays)

> **文档状态**: [DESIGN]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-10  
> **定位**: 用数据驱动建模 WoW 级世界内容节奏——节日、世界事件、入侵、场景战役。每事件有周期时间表、受影响区域、关联任务/boss、专属币种与奖励。  
> **配套契约**: `proto/protobuf/config_world_events.proto`（config-center 热更，`*ConfigSet{data_version}`）  
> **联动**: `config_zones`（受影响区域）、`config_spawns`（事件期间改写生物分布）、`config_quests`/`config_creatures`/`config_currencies`（奖励与 boss）。  
> **平衡校验**: 见 §5，呼应 `systems-catalog.md §5` 经济源汇总纲。

---

## 1. 设计原则

1. **时间驱动、可调度**：事件完全由 `ScheduleRule` 描述（一次性/每年/每月/每周 + 时段），调度器离线排程，零代码改动即可新增节日。
2. **区域/内容解耦**：事件只声明"影响哪些 `zone_id`、挂哪些 `quest_id`/`creature_id`"，不内嵌任何区域几何。
3. **币种闭环**：事件专属币种 (`event_currency_id`) 走 `config_currencies`，其产出速率由 `WorldEventBalance` 校验，纳入经济源汇对账（避免节日币种通胀）。
4. **与刷新联动**：事件期间的特殊生物由 `config_spawns.SpawnCondition.required_event_ids` 引用本事件，不改 Spawn 静态表即可"临时改写"分布。

---

## 2. 契约结构（config_world_events.proto）

| 消息 | 作用 | 关键字段 |
|------|------|---------|
| `WorldEventCategory` | 事件分类 | HOLIDAY / WORLD / INVASION / SCENARIO |
| `ScheduleRule` | 周期时间表 | start/end_date、start_hour、duration_hours、recurrence(0/1/2/3)、weekday |
| `WorldEventBalance` | 平衡校验 | expected_currency_per_hour / tolerance_pct / alert_channel |
| `WorldEventConfig` | 事件定义 | event_id / category / schedule / affected_zone_ids / quest_ids / creature_ids / event_currency_id / rewards(ItemReward) / script_ref / balance |
| `WorldEventConfigSet` | 加载/热更单元 | data_version / events |

**跨集引用裸 `uint32`**（防环）：zone_ids→`config_zones`；quest_ids→`config_quests`；creature_ids→`config_creatures`；event_currency_id→`config_currencies`。仅 `import "common.proto"` 复用 `ItemReward`。

---

## 3. 联动关系

```
[WorldEvent] --affected_zone_ids--> [ZoneConfig]
     |--quest_ids--> [QuestConfig]
     |--creature_ids--> [CreatureTemplate]  (事件 boss/稀有)
     |--event_currency_id--> [CurrencyConfig]
     |                      (玩家赚事件币 -> 商店花 -> 经济闭环)
     └--(被引用)-- [SpawnCondition.required_event_ids] --> [SpawnEntry]  (事件期特怪)
```

---

## 4. 运行时映射

| 契约 | 运行时 |
|------|--------|
| `ScheduleRule` | 全局事件调度器：离线展开为时间轴，到点向受影响区域的 Scene 广播 `WorldEventStart/End` 事件（同场景 EventBus，见 `scene.md §5`） |
| `affected_zone_ids` | 仅这些 Zone 的 Scene 实例加载事件脚本/任务/NPC |
| `script_ref` | 事件特殊逻辑（boss 周期刷新、阶段切换）在 Lua 边界，schema 只存引用 |
| `rewards` / `event_currency_id` | 任务/成就结算经 economy 模块发奖（ADR-002：economy 为唯一发奖出口） |

---

## 5. 平衡校验路径

呼应专家职责——把"节日好玩"落成可验证产物。事件币是**经济源**，必须和经济汇对账：

| 编号 | 校验对象 | 方法 | 验证路径 |
|------|---------|------|---------|
| V1 | 事件币产出速率 | 离线模拟：事件时长 × 人均完成率 × 单位奖励，断言 `≈ WorldEventBalance.expected_currency_per_hour`，偏差 > tolerance 告警 | 每个事件带 `balance` 条目 |
| V2 | 事件币总注入 | 线上按等级带统计日产出，与 `config_balance.EconomyFlow`（事件币对应的源条目）对账，监控泄漏/囤积率 | 复用 §5.1 经济源汇双轨 |
| V3 | 参与率健康度 | 线上统计各事件参与人数/完成率，过低则调奖励或时间表 | 监控面板（`systems-catalog §5` 工程落地） |

---

## 6. 开放问题 / 后续

- 事件专属 NPC 商人（引用 `config_vendors`）与事件商店：待 vendors 扩展事件币种价目。
- 事件进度存档与跨年重复：存档键设计待细化。
- 与 Spawns 的深度联动（事件期刷怪密度曲线）需在 `SpawnDensityBalance` 上加事件维度。
