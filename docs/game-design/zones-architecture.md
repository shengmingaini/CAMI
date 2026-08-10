# 区域系统架构设计 (Zones / Gates / Instances)

> **文档状态**: [DESIGN]  
> **版本**: v1.0.0  
> **更新日期**: 2026-08-10  
> **定位**: 把"区域"建模为**相互独立、模块化**的内容单元，区域之间**仅通过门（Gate）连接**；副本 / 团本是**独立区域**（类型不同，另载实例规则）。  
> **配套契约**: `proto/protobuf/config_zones.proto`（数据驱动，config-center 热更，含 `data_version`）  
> **运行时映射**: `docs/modules/scene.md` + ADR-012（场景级线程隔离）/ ADR-016（Cell 开放世界）/ ADR-010（Phasing）  
> **平衡校验**: 见 §6，呼应 `systems-catalog.md §5` 总纲与系统设计师"每条数值配验证路径"的职责。

---

## 1. 设计原则（用户硬约束）

1. **区域相互独立、模块化**  
   `ZoneConfig` 只描述"自身"（类型、等级带、包围盒、相位、复活点），**不硬编码任何邻居**。每个区域是独立加载/卸载/线程隔离单元。
2. **门是唯一连通方式**  
   区域之间的转移**只能**由 `GateConfig` 表达，构成一张**有向图**。没有门，就没有连通——这让"加一块新地图"变成纯数据工作，零代码改动。
3. **副本 / 团本即独立区域**  
   副本、团本以 `ZoneType = DUNGEON / RAID` 的 `ZoneConfig` 存在；其"实例专属规则"（队伍上限、锁期、难度倍率、boss 引用）由 `InstanceConfig` 承载；进入仍是一扇 `Gate`（入口门）。
4. **数据驱动、策划可调**  
   全部走 `ZoneConfigSet{data_version}`，config-center 热更——改地图拓扑/门条件不动代码。

---

## 2. 契约结构（config_zones.proto）

| 消息 | 作用 | 关键字段 |
|------|------|---------|
| `ZoneConfig` | 模块化独立区域 | `zone_id` / `zone_type` / `min_level`·`max_level`（等级带）/ `bounds_min`·`bounds_max`（包围盒）/ `graveyard` / `phase_group_ids` / `pvp_mode` / `linked_instance_ids` |
| `GateConfig` | 门（区域间唯一连通） | `source_zone_id`·`source_position` / `target_zone_id`·`target_position` / `bidirectional` / `requirement` / `cooldown_ms` / `script_ref` |
| `GateRequirement` | 进入条件 | `min_level`·`max_level` / `required_quest_ids` / `required_item_ids` / `required_faction_ids`(+`standing`) / `required_currency_*` / `required_script_flag` |
| `InstanceConfig` | 副本/团本（独立区域 + 实例规则） | `zone_id`（指向 DUNGEON/RAID 区域）/ `max_players` / `difficulties` / `boss_creature_ids` / `reset_period_hours` / `entrance_gate_id` / `is_heroic` |
| `InstanceDifficulty` | 难度档 | `health_mod` / `damage_mod` / `loot_quality_mod` / `min_level` |
| `ZoneConfigSet` | 加载/热更单元 | `data_version` / `zones` / `gates` / `instances` |

**跨集引用一律裸 `uint32` ID（防 import 环）**：`boss_creature_ids→config_creatures`、`required_faction_ids→config_reputation`、`required_currency_id→config_currencies`、`required_quest/item_ids→config_quests/config_items`。仅 `import "common.proto"` 复用 `Vec3`。

---

## 3. 门拓扑（有向图）

```
[野外区A] --Gate(g1, 双向)--> [主城B]
   |
   +--Gate(g2, 单向, 需任务X)--> [副本入口区C] --Gate(entrance)--> [副本实例D: RAID]
[主城B] --Gate(g3)--> [野外区E]
```

- **有向 + 可选双向**：`bidirectional=false` 时只许 source→target；`=true` 时 target→source 也通（副本出口门通常只出不进，避免误出再误入）。
- **进入条件在目标侧**：`GateRequirement` 校验的是"能否进入 target_zone"，与反向无关。
- **冷却防滥用**：`cooldown_ms` 防 AOE 跳关 / 刷本秒进秒出。
- **脚本门**：`script_ref` 承载剧情锁门（如"黑暗之门未开启则不可通行"），逻辑在 Lua 边界，schema 只存引用。
- **模块化保证**：新增区域 = 新增一条 `ZoneConfig` + 若干 `GateConfig`；旧区域无需改动一行。

---

## 4. 副本 / 团本即独立区域

- **区域身份**：副本/团本首先是 `ZoneConfig`（`zone_type=DUNGEON/RAID`），拥有自己的包围盒、等级带、相位、复活点。
- **实例规则**：`InstanceConfig.zone_id` 指向该区域，额外声明：
  - `max_players`（副本 5 / 团本 10·25）——决定按队伍拉起几个独立 Scene 实例；
  - `difficulties`（英雄/普通等，倍率叠加到 boss 与小怪）；
  - `boss_creature_ids`（引用 `config_creatures` 的 boss 模板）；
  - `reset_period_hours`（锁期）；
  - `entrance_gate_id`（入口门，进入条件在此校验）。
- **隔离性**：每次组队进入，调度器按 `entrance_gate_id` 拉起一个**全新、独立**的 Scene 实例（ADR-012），彼此不共享状态；锁期到 / 全队离场后回收。

---

## 5. 运行时映射（World ↔ Scene）

| 契约 | 运行时 | 说明 |
|------|--------|------|
| `ZoneConfig` | `SceneInstance`（每区域一个，独立线程；大世界按 Cell 再分线程，ADR-016） | `bounds_*` → Cell 划分依据；`pvp_mode` → 场景 PvP 规则；`phase_group_ids` → 相位索引（ADR-010） |
| `GateConfig` | 跨场景迁移 | 玩家抵达 `source_position` 触发 → 状态序列化经 MPMC 无锁队列 → 在 `target_zone` 的 `target_position` 重生 |
| `InstanceConfig` | 按 group 独立 Scene 实例 | 入口门触发 → 调度器创建隔离实例；`max_players` 限流；`reset_period_hours` 驱动回收 |

> 这与 `scene.md §4` 的 `SceneConfig{kind(主城/野外/副本)}` 完全对应；本契约把"哪些区域、怎么连通、副本怎么锁"从代码下沉为数据，scene 模块只消费 `ZoneConfigSet`。

---

## 6. 平衡校验路径（每条数值可验证）

呼应专家职责——把"好玩"落成可交付的工程产物。区域/副本的下列数值必须可观测、可模拟、可告警：

| 编号 | 校验对象 | 方法 | 验证路径 |
|------|---------|------|---------|
| V1 | 等级带一致性 | 离线断言 `ZoneConfig.min/max_level` 与区域内预期 `config_creatures` 等级、`config_quests` 等级重叠；偏差告警 | 每个 Zone 带 `min_level/max_level`，离线校验器读取 creatures/quests 等级分布对比 |
| V2 | 门可达性 / 无死区 | 构图后跑连通性：从出生点出发，所有 `ZoneConfig` 必须可达；`GateRequirement` 不得造成"进得去出不来" | 离线图遍历（有向图 BFS），输出孤儿区域 / 单向陷阱清单 |
| V3 | 副本难度倍率 | `InstanceDifficulty.{health,damage,loot_quality}_mod` 喂入 DPS/TTK 蒙特卡洛（复用 `config_balance` + `config_stats`），断言 TTK ∈ 目标区间、各职业方差 < 阈值 | 模拟器读 `InstanceConfig` + boss 模板，产出 TTK 报告 |
| V4 | 锁期 × 队伍规模 | `reset_period_hours × max_players` 估算副本并发实例数，对比场景线程/内存预算（ADR-012 容量） | 容量规划器读 `InstanceConfig` 生成分片建议 |

> 具体模拟器/监控面板属工程落地，不在本文范围；本文负责把"需要被验证的数值"显式建模进 schema（`min_level`/`max_level`/`difficulties`/`reset_period_hours` 等）。

---

## 7. 开放问题 / 后续

- **刷新/种群 Spawns**（待建 #2）：`SpawnConfig` 将引用 `ZoneConfig.zone_id` + 落入 `bounds_*` 内坐标，复用本契约的模块化边界。
- **天气系统**：`weather_ids` 预留，weather 预设 schema 待建。
- **相位细节**：`phase_group_ids` 与 `scene.md` ADR-010 的相位规则对齐，相位切换语义待细化。
- **PvP 场景**：`ZoneType=BATTLEGROUND/ARENA` 已预留，仍缺队列 / 锁期 / 军衔 / 赛季（`systems-catalog §4 #3`）。
