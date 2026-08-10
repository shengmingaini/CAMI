# CAMI 游戏机制设计 (Game Design)

> 以系统思维拆解达到 **WoW 复杂度** 的游戏机制，建模大量预设参数，并为每条数值配平衡验证路径——把"好玩"落成可交付的工程产物。

## 文档

| 文件 | 说明 |
|------|------|
| `systems-catalog.md` | **游戏机制系统总目**：全部系统按支柱（角色/战斗/经济/世界/社交/PvP）分组、目标参数量级（WoW 参考：数十万~百万行）、预设参数类别、Day 3 状态与缺口、本批 config 契约、待建系统、平衡校验总纲（§5） |
| `zones-architecture.md` | **区域系统架构**：模块化独立区域 + 门连接拓扑 + 副本团本即独立区域，运行时映射（Zone=Scene/ADR-012·16）、平衡校验路径（§6） |
| `world-events-design.md` | **世界事件/节日架构**：周期时间表 + 受影响区域/任务/boss/币种 + 与 Spawns 联动 + 事件币经济源汇校验 |
| `spawns-design.md` | **刷新/种群架构**：刷新点落于区域边界 + 刷新池 + 条件(等级/相位/事件/时段) + 密度↔经济源平衡 |
| `pvp-design.md` | **PvP 全量架构**：战场/竞技场/赛季/军衔/MMR + 复用 Zones 与 Currencies + 荣誉·征服产出与周上限平衡 |
| `balance-verification.md` | **平衡校验报告**：基础内容填充 + `verify/balance_verifier.py` 校验结果（源汇对账/周上限/密度/引用完整性/门可达性）、运行方式、修复记录 |

## 配套契约（数据驱动预设参数，`CAMI.Config`，config-center 热更）

| 文件 | 覆盖系统 |
|------|---------|
| `../proto/protobuf/config_stats.proto` | 属性系统：rating→% 曲线 / 物品化预算 / 职业权重 / 衍生换算 |
| `../proto/protobuf/config_balance.proto` | 战斗常数 + 等级 XP 曲线 + **经济源汇**(闭合 GAP-4) |
| `../proto/protobuf/config_loot.proto` | 掉落表 / 品质权重 / 分配模式 / Bonus Roll |
| `../proto/protobuf/config_creatures.proto` | 生物/NPC 模板：属性预设 / 技能组 / AI / 掉落引用 / 缩放 |
| `../proto/protobuf/config_reputation.proto` | 声望阵营：档位阈值 / 折扣 / 来源日上限 |
| `../proto/protobuf/config_progression.proto` | 天赋树 / 专精 / 成就 / 称号 |
| `../proto/protobuf/config_zones.proto` | 区域 / 门（连接拓扑）/ 副本·团本（独立区域 + 锁期/难度/boss） |
| `../proto/protobuf/config_world_events.proto` | 世界事件 / 节日（时间表/受影响区域/任务/boss/专属币种/奖励） |
| `../proto/protobuf/config_spawns.proto` | 刷新 / 种群（刷新点/池/条件/密度平衡↔经济源） |
| `../proto/protobuf/config_pvp.proto` | PvP 全量（战场/竞技场/赛季/军衔/MMR/荣誉·征服平衡） |

> 方案 B 已建（同属数据驱动）：`config_items` / `config_skills` / `config_quests`。

## 基础内容与平衡校验（可运行工程）

| 路径 | 说明 |
|------|------|
| `../../data/configs/*.json` | 13 个 ConfigSet 的**基础内容填充**（货币/物品/掉落/生物/刷新/商店/PvP/世界事件/声望/专业/区域/属性/战斗平衡），构成一个自洽小世界，可直接被校验器加载 |
| `../../verify/balance_verifier.py` | **平衡校验引擎**：经济源汇对账（GAP-4 闭环）、币种周上限三处对账、刷新密度↔掉落金、世界事件币、PvP 产出、专业净注入、引用完整性、区域等级带、门可达性、结构单调性；退出码 0/1 可接入 CI |
| `../../verify/load_configs.py` | 配置加载器（JSON→Protobuf；桩缺失时自动 `protoc` 重新生成） |
| `../../verify/balance_report.txt` | 最近一次校验报告文本 |

**运行**：`python verify/balance_verifier.py --data data/configs`（受管 venv：`C:/Users/17283/.workbuddy/binaries/python/envs/default/Scripts/python.exe`）。

## 设计原则

1. **数据驱动、策划可调**：所有数值/规则走 `config_*.proto` 的 `*ConfigSet`（含 `data_version`），经 config-center 热更——改数值不动代码。
2. **数值不进逻辑代码**：伤害/缩放等公式由 `config_balance` 常数 + Lua 热更边界承担（沿用 `combat.md §1.2`）。
3. **平衡可验证（已实现离线）**：每条关键数值（掉率/汇率/经验/缩放）配校验路径，离线校验见 `verify/balance_verifier.py`，线上监控为后续项，见 `systems-catalog.md §5`。
